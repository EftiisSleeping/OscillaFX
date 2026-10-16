// UI preview only: stands in for platform/<os>/MicrophoneAccess_<os> so the panel's "allow microphone" state
// can be previewed without touching the system permission. OSCILLA_STUB_MIC_DENIED=1 -> denied.
#ifndef OSCILLA_UI_STUB
 #define OSCILLA_UI_STUB 0
#endif

#if OSCILLA_UI_STUB

#include "platform/MicrophoneAccess.h"
#include <juce_events/juce_events.h>

namespace MicrophoneAccess
{
Status status()
{
    return juce::SystemStats::getEnvironmentVariable ("OSCILLA_STUB_MIC_DENIED", {}).isNotEmpty() ? Status::denied
                                                                                                    : Status::granted;
}

void request (std::function<void (bool)> onResult)
{
    juce::MessageManager::callAsync ([onResult] { onResult (status() == Status::granted); });
}

void openSystemSettings()
{
    DBG ("MicrophoneAccess stub: would open System Settings > Privacy & Security > Microphone");
}
} // namespace MicrophoneAccess

#endif
