#include "platform/win/WicImage.h"
#include "platform/win/ImageHeader.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>

#pragma comment (lib, "windowscodecs.lib")
#pragma comment (lib, "ole32.lib")

namespace WicImage
{
namespace
{
using Microsoft::WRL::ComPtr;

// Not in every Windows SDK's wincodec.h.
constexpr GUID CONTAINER_WEBP = { 0xe094b0e2, 0x67f2, 0x45b3, { 0xb0, 0xea, 0x11, 0x53, 0x37, 0xca, 0x7c, 0xf3 } };
constexpr GUID CONTAINER_HEIF = { 0xe1e62521, 0x6787, 0x405b, { 0xa3, 0x39, 0x50, 0x07, 0x15, 0xb5, 0x76, 0x3f } };

// Joins whatever apartment the thread has; a thread already in an STA (message thread) is fine.
class ScopedCom
{
public:
    ScopedCom() : result (CoInitializeEx (nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedCom() { if (SUCCEEDED (result)) CoUninitialize(); }
    ScopedCom (const ScopedCom&) = delete;
    ScopedCom& operator= (const ScopedCom&) = delete;

private:
    HRESULT result;
};

struct Source
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    GUID container {};
};

ComPtr<IWICImagingFactory> createFactory()
{
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&factory));
    return factory;
}

Source open (const juce::File& file)
{
    Source source;
    source.factory = createFactory();
    if (source.factory == nullptr
        || FAILED (source.factory->CreateDecoderFromFilename (file.getFullPathName().toWideCharPointer(), nullptr, GENERIC_READ,
                                                              WICDecodeMetadataCacheOnDemand, &source.decoder))
        || FAILED (source.decoder->GetContainerFormat (&source.container))
        || FAILED (source.decoder->GetFrame (0, &source.frame)))
        source.frame = nullptr;
    return source;
}

const char* extensionFor (const GUID& container)
{
    struct Name { const GUID& container; const char* extension; };
    const Name names[] = {
        { GUID_ContainerFormatJpeg, "jpg" }, { GUID_ContainerFormatPng, "png" }, { CONTAINER_WEBP, "webp" },
        { CONTAINER_HEIF, "heic" },          { GUID_ContainerFormatGif, "gif" }, { GUID_ContainerFormatTiff, "tiff" },
    };
    for (const auto& name : names)
        if (IsEqualGUID (container, name.container))
            return name.extension;
    return "";
}

WICBitmapTransformOptions transformForExif (USHORT orientation)
{
    switch (orientation)
    {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return (WICBitmapTransformOptions) (WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return (WICBitmapTransformOptions) (WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

WICBitmapTransformOptions orientationOf (IWICBitmapFrameDecode& frame)
{
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED (frame.GetMetadataQueryReader (&reader)))
        return WICBitmapTransformRotate0;
    for (const auto* query : { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" })
    {
        PROPVARIANT value;
        PropVariantInit (&value);
        const bool found = SUCCEEDED (reader->GetMetadataByName (query, &value)) && value.vt == VT_UI2;
        const auto orientation = found ? value.uiVal : (USHORT) 0;
        PropVariantClear (&value);
        if (found)
            return transformForExif (orientation);
    }
    return WICBitmapTransformRotate0;
}

// frame -> (scaler) -> (flip/rotate) -> premultiplied BGRA
ComPtr<IWICBitmapSource> uprightScaled (Source& source, UINT width, UINT height, int maxSide)
{
    ComPtr<IWICBitmapSource> stage = source.frame;
    const auto scale = juce::jmin (1.0, (double) maxSide / juce::jmax (width, height));
    if (scale < 1.0)
    {
        ComPtr<IWICBitmapScaler> scaler;
        const auto scaledWidth = (UINT) juce::jmax (1, juce::roundToInt (width * scale));
        const auto scaledHeight = (UINT) juce::jmax (1, juce::roundToInt (height * scale));
        if (FAILED (source.factory->CreateBitmapScaler (&scaler))
            || FAILED (scaler->Initialize (stage.Get(), scaledWidth, scaledHeight, WICBitmapInterpolationModeFant)))
            return nullptr;
        stage = scaler;
    }
    if (const auto transform = orientationOf (*source.frame.Get()); transform != WICBitmapTransformRotate0)
    {
        ComPtr<IWICBitmapFlipRotator> rotator;
        if (SUCCEEDED (source.factory->CreateBitmapFlipRotator (&rotator)) && SUCCEEDED (rotator->Initialize (stage.Get(), transform)))
            stage = rotator;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED (source.factory->CreateFormatConverter (&converter))
        || FAILED (converter->Initialize (stage.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom)))
        return nullptr;
    return converter;
}

juce::uint8 luminance (const BYTE* bgra)
{
    return (juce::uint8) juce::roundToInt (0.0722f * bgra[0] + 0.7152f * bgra[1] + 0.2126f * bgra[2]);
}

bool startsWith (const juce::MemoryBlock& bytes, size_t offset, const char* text)
{
    const auto length = std::strlen (text);
    return bytes.getSize() >= offset + length && std::memcmp ((const char*) bytes.getData() + offset, text, length) == 0;
}
} // namespace

Info probe (const juce::File& file)
{
    ScopedCom com;
    auto source = open (file);
    Info info;
    UINT width = 0, height = 0;
    if (source.frame == nullptr || FAILED (source.frame->GetSize (&width, &height)))
        return info;
    info.isDecodable = true;
    info.extension = extensionFor (source.container);
    info.width = width;
    info.height = height;
    return info;
}

Grey decodeGrey (const juce::File& file, int maxSide)
{
    ScopedCom com;
    auto source = open (file);
    UINT width = 0, height = 0;
    if (source.frame == nullptr || FAILED (source.frame->GetSize (&width, &height)) || width == 0 || height == 0
        || (juce::int64) width * height > ImageHeader::MAX_PIXELS)
        return {};
    auto pixels = uprightScaled (source, width, height, juce::jmax (1, maxSide));
    UINT outWidth = 0, outHeight = 0;
    if (pixels == nullptr || FAILED (pixels->GetSize (&outWidth, &outHeight)))
        return {};

    const auto stride = outWidth * 4;
    std::vector<BYTE> bgra ((size_t) stride * outHeight);
    if (FAILED (pixels->CopyPixels (nullptr, stride, (UINT) bgra.size(), bgra.data())))
        return {};

    Grey grey { (int) outWidth, (int) outHeight, {} };
    grey.pixels.resize ((size_t) outWidth * outHeight);
    for (size_t i = 0; i < grey.pixels.size(); ++i)
        grey.pixels[i] = luminance (bgra.data() + i * 4); // premultiplied: already times alpha
    return grey;
}

bool hasCodec (const juce::String& extension)
{
    const GUID* container = extension == "webp" ? &CONTAINER_WEBP : extension == "heic" ? &CONTAINER_HEIF : nullptr;
    if (container == nullptr)
        return true; // the others are built into Windows
    ScopedCom com;
    auto factory = createFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    return factory != nullptr && SUCCEEDED (factory->CreateDecoder (*container, nullptr, &decoder));
}

juce::String sniffOptionalFormat (const juce::File& file)
{
    juce::MemoryBlock head;
    if (juce::FileInputStream stream (file); stream.openedOk())
        stream.readIntoMemoryBlock (head, 16);
    if (startsWith (head, 0, "RIFF") && startsWith (head, 8, "WEBP"))
        return "webp";
    if (startsWith (head, 4, "ftyp"))
        for (const auto* brand : { "heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1" })
            if (startsWith (head, 8, brand))
                return "heic";
    return {};
}
} // namespace WicImage
