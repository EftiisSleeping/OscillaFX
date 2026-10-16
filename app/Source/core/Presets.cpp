#include "Presets.h"
#include "AppPaths.h"
#include "audio/DspParams.h"
#include "DfxDsp.h"

namespace
{
const char* const PRESET_EXTENSION = ".fac";
constexpr int PRESET_READ_SAMPLE_RATE = 48000;

const char* const FEATURED_PRESETS[] = { "Movies", "Music" }; // Oscilla's own, listed first

// <root>/oscilla/app/Source/core/Presets.cpp -> <root>/oscilla
juce::File sourceTree()
{
    return juce::File (__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getParentDirectory();
}

juce::File findPresetFile (const juce::String& name)
{
    for (const auto& dir : Presets::directories())
        if (auto file = dir.getChildFile (name + PRESET_EXTENSION); file.existsAsFile())
            return file;
    return {};
}

void moveFeaturedToFront (juce::StringArray& names)
{
    int insertAt = 0;
    for (auto* featured : FEATURED_PRESETS)
        if (names.contains (featured))
            names.move (names.indexOf (featured), insertAt++);
}
} // namespace

namespace Presets
{
juce::Array<juce::File> directories()
{
    const juce::File candidates[] = {
        AppPaths::resourcesDirectory().getChildFile ("Presets"),
        sourceTree().getChildFile ("presets"),
        sourceTree().getChildFile ("presets_fxsound"),
    };
    juce::Array<juce::File> result;
    for (const auto& dir : candidates)
        if (dir.isDirectory())
            result.add (dir);
    return result;
}

juce::StringArray names()
{
    juce::StringArray result;
    for (const auto& dir : directories())
        for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, juce::String ("*") + PRESET_EXTENSION))
            result.addIfNotAlreadyThere (file.getFileNameWithoutExtension());
    result.sortNatural();
    moveFeaturedToFront (result);
    return result;
}

// A fresh, zeroed DfxDsp per load keeps the result independent of previous calls.
std::optional<DspSettings> load (const juce::String& name, const DspSettings& base)
{
    auto file = findPresetFile (name);
    if (file == juce::File())
        return std::nullopt;

    DfxDsp dsp;
    dsp.setSignalFormat (32, 2, PRESET_READ_SAMPLE_RATE, 32);
    DspParamsApply::configureBands (dsp);
    auto neutral = DspParams::fromSettings (DspSettings {});
    neutral.effects = {};
    neutral.eqGainDb = {};
    DspParamsApply::applyAll (dsp, neutral);
    if (dsp.loadPreset (file.getFullPathName().toWideCharPointer()) != 0)
        return std::nullopt;

    DspSettings result = base;
    DspParamsApply::readInto (dsp, result);
    result.presetName = name;
    return result;
}
} // namespace Presets
