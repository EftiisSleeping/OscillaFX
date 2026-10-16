#include "ui/WindowChrome.h"

// Windows keeps the native title bar above the panel; there is no haptic trackpad API.
namespace WindowChrome
{
void useFullSizeContent (juce::ComponentPeer&) {}

void detentHaptic() {}
}
