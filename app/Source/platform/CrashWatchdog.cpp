#include "platform/CrashWatchdog.h"
#include "core/AppPaths.h"

namespace CrashWatchdog
{
// Fixed location (never OSCILLA_DATA_DIR): the helper is given this path when it is spawned.
juce::File restoreFile()
{
    return AppPaths::defaultDataDirectory().getChildFile ("restore_output_uid.txt");
}

bool writeRestoreUid (const juce::String& uid)
{
    auto file = restoreFile();
    return file.getParentDirectory().createDirectory()
        && file.replaceWithText (juce::String (currentProcessId()) + "\n" + uid + "\n", false, false, "\n"); // helper parses LF
}

juce::String readRestoreUid()
{
    juce::StringArray lines;
    lines.addLines (restoreFile().loadFileAsString().trim());
    return lines.isEmpty() ? juce::String() : lines[lines.size() - 1].trim();
}

void clearRestoreUid() { restoreFile().deleteFile(); }
} // namespace CrashWatchdog
