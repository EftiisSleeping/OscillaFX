#include "platform/MicrophoneAccess.h"

#include <juce_events/juce_events.h>
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

namespace MicrophoneAccess
{
Status status()
{
   #if OSCILLA_UI_STUB
    return Status::granted; // preview build captures nothing; never prompt
   #else
    switch ([AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:    return Status::granted;
        case AVAuthorizationStatusNotDetermined: return Status::undetermined;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:    return Status::denied;
    }
    return Status::denied;
   #endif
}

void request (std::function<void (bool)> onResult)
{
    [AVCaptureDevice requestAccessForMediaType: AVMediaTypeAudio
                             completionHandler: ^(BOOL granted) {
                                 juce::MessageManager::callAsync ([onResult, granted] { onResult (granted); });
                             }];
}

void openSystemSettings()
{
    NSURL* url = [NSURL URLWithString: @"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"];
    if (url != nil)
        [NSWorkspace.sharedWorkspace openURL: url];
}
} // namespace MicrophoneAccess
