#pragma once

#include "ui/crt/CrtScene.h"
#include <map>
#include <mutex>
#include <set>

// Pictures for the phosphor gallery. Sources, merged and sorted by file name:
//   $OSCILLA_GALLERY_DIR (if set, used alone), GalleryStore::defaultUserDirectory(),
//   AppPaths::resourcesDirectory()/Gallery; when those hold nothing, <repo>/assets/gallery (dev).
// Pictures are decoded on a background thread straight to a <= 512 px thumbnail, converted to
// luminance and auto-levelled. macOS decodes anything ImageIO reads (png / jpg / webp / heic ...);
// Windows decodes what JUCE reads (png / jpg / gif).
// Message thread only, apart from the internal decoder.
namespace Crt
{
    class Gallery
    {
    public:
        Gallery();
        ~Gallery();

        void rescan (bool force = false);   // cheap to call often: re-lists the folders at most every couple of seconds unless forced
        int size() const { return files.size(); }

        // The picture, or nullptr while it is still being decoded (or cannot be decoded).
        const LuminanceImage* get (int index);

        static juce::File userDirectory();

    private:
        struct Inbox
        {
            std::mutex lock;
            std::vector<std::pair<juce::String, LuminanceImage>> decoded;
        };

        void collectDecoded();

        juce::Array<juce::File> files;
        std::map<juce::String, LuminanceImage> cache;   // by full path; a failed decode stays invalid
        std::set<juce::String> pending;
        std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
        juce::ThreadPool decoder { juce::ThreadPoolOptions{}.withThreadName ("Gallery decoder").withNumberOfThreads (1) };
        juce::uint32 lastScanMs = 0;
        bool hasScanned = false;
        int nextId = 1;
    };

    // Per platform (ui/crt/<os>/GalleryDecode_<os>): decodes a picture into luminance, as a
    // thumbnail whose longer side is at most maxSide pixels. Thread-safe.
    LuminanceImage decodeLuminance (const juce::File&, int maxSide);
}
