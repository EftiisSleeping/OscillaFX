#pragma once

#include <juce_core/juce_core.h>

namespace AppPaths
{
// macOS: ~/Library/Application Support/Oscilla (pre-rename name, kept so existing profiles stay).
// Windows: %APPDATA%\OscillaFX.
inline juce::File defaultDataDirectory()
{
    const auto root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    return root.getChildFile ("Application Support/Oscilla");
   #else
    return root.getChildFile ("OscillaFX");
   #endif
}

// defaultDataDirectory(), or $OSCILLA_DATA_DIR when set. The override keeps preview builds,
// screenshots and experiments away from the real profiles, prefs and gallery.
inline juce::File dataDirectory()
{
    const auto overridePath = juce::SystemStats::getEnvironmentVariable ("OSCILLA_DATA_DIR", {});
    if (overridePath.isNotEmpty() && juce::File::isAbsolutePath (overridePath))
        return juce::File (overridePath);
    return defaultDataDirectory();
}

// Read-only files shipped with the app (Presets/, Gallery/).
// macOS: OscillaFX.app/Contents/Resources. Windows: the folder holding OscillaFX.exe.
inline juce::File resourcesDirectory()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources");
   #else
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
   #endif
}
} // namespace AppPaths
