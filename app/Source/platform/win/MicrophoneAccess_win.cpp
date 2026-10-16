#include "platform/MicrophoneAccess.h"

#include <juce_events/juce_events.h>

// Capturing the virtual endpoint through WASAPI needs no consent prompt on desktop Windows apps
// unless the user turned off "Let desktop apps access your microphone"; treat it as granted.
namespace MicrophoneAccess
{
Status status() { return Status::granted; }

void request (std::function<void (bool)> onResult)
{
    juce::MessageManager::callAsync ([onResult = std::move (onResult)] { onResult (true); });
}

void openSystemSettings()
{
    juce::URL ("ms-settings:privacy-microphone").launchInDefaultBrowser();
}
} // namespace MicrophoneAccess
