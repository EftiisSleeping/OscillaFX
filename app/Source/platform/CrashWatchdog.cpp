#include "platform/CrashWatchdog.h"
#include "core/AppPaths.h"

namespace CrashWatchdog
{
// Fixed location (never OSCILLA_DATA_DIR): the helper is given this path when it is spawned.
juce::File restoreFile()
{
    return AppPaths::defaultDataDirectory().getChildFile ("restore_output_uid.txt");
}

namespace
{
bool writeRecord (const juce::String& body)
{
    auto file = restoreFile();
    return file.getParentDirectory().createDirectory()
        && file.replaceWithText (juce::String (currentProcessId()) + "\n" + body, false, false, "\n"); // helper parses LF
}

juce::StringArray recordLines()
{
    juce::StringArray lines;
    lines.addLines (restoreFile().loadFileAsString().trim());
    return lines;
}
} // namespace

bool writeRestoreUid (const juce::String& uid) { return writeRecord (uid + "\n"); }

bool writeRestoreUid (const juce::String& uid, int roles)
{
    return writeRecord (uid + "\n" + juce::String (roles & ALL_ROLES) + "\n");
}

// "<uid>" (legacy), "<pid>\n<uid>" or "<pid>\n<uid>\n<roles>"
juce::String readRestoreUid()
{
    const auto lines = recordLines();
    return (lines.size() >= 2 ? lines[1] : lines[0]).trim();
}

int readRestoreRoles()
{
    const auto lines = recordLines();
    const auto roles = lines.size() >= 3 ? lines[2].trim().getIntValue() & ALL_ROLES : 0;
    return roles != 0 ? roles : DEFAULT_ROLES;
}

void clearRestoreUid() { restoreFile().deleteFile(); }
} // namespace CrashWatchdog
