#include "platform/LoginItem.h"

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

namespace LoginItem
{
namespace
{
NSString* const LOGIN_ARGUMENT = @"--launched-at-login";
NSString* const AGENT_LABEL = @"com.oscilla.app.login";

// ---------------------------------------------------------------- SMAppService (macOS 13+)

enum class ServiceState { unavailable, off, awaitingApproval, enabled };

ServiceState serviceState()
{
    if (@available (macOS 13.0, *))
    {
        switch (SMAppService.mainAppService.status)
        {
            case SMAppServiceStatusEnabled:          return ServiceState::enabled;
            case SMAppServiceStatusRequiresApproval: return ServiceState::awaitingApproval;
            case SMAppServiceStatusNotRegistered:
            case SMAppServiceStatusNotFound:         return ServiceState::off;
        }
        return ServiceState::off;
    }
    return ServiceState::unavailable;
}

// True if the service ends up enabled (registration can leave it awaiting the user's approval).
bool registerService()
{
    if (@available (macOS 13.0, *))
    {
        NSError* error = nil;
        if (! [SMAppService.mainAppService registerAndReturnError: &error])
            NSLog (@"OscillaFX: SMAppService register failed: %@", error);
    }
    return serviceState() == ServiceState::enabled;
}

void unregisterService()
{
    if (@available (macOS 13.0, *))
    {
        NSError* error = nil;
        if (! [SMAppService.mainAppService unregisterAndReturnError: &error])
            NSLog (@"OscillaFX: SMAppService unregister failed: %@", error);
    }
}

void openLoginItemsSettings()
{
    if (@available (macOS 13.0, *))
        [SMAppService openSystemSettingsLoginItems];
}

// ---------------------------------------------------------------- LaunchAgent fallback

NSString* agentPath()
{
    return [NSHomeDirectory() stringByAppendingFormat: @"/Library/LaunchAgents/%@.plist", AGENT_LABEL];
}

bool isAgentInstalled() { return [NSFileManager.defaultManager fileExistsAtPath: agentPath()]; }

bool installAgent()
{
    NSString* executable = NSBundle.mainBundle.executablePath;
    if (executable == nil)
        return false;
    NSDictionary* plist = @{
        @"Label" : AGENT_LABEL,
        @"ProgramArguments" : @[ executable, LOGIN_ARGUMENT ],
        @"RunAtLoad" : @YES,
        @"ProcessType" : @"Interactive",
        @"LimitLoadToSessionType" : @"Aqua",
    };
    NSString* path = agentPath();
    [NSFileManager.defaultManager createDirectoryAtPath: path.stringByDeletingLastPathComponent
                            withIntermediateDirectories: YES attributes: nil error: nil];
    NSError* error = nil;
    if (! [plist writeToURL: [NSURL fileURLWithPath: path] error: &error])
        NSLog (@"OscillaFX: cannot write %@: %@", path, error);
    return isAgentInstalled();
}

void removeAgent() { [NSFileManager.defaultManager removeItemAtPath: agentPath() error: nil]; }

// After the Oscilla -> OscillaFX rename an agent written by the old app still points at
// Oscilla.app/Contents/MacOS/Oscilla; rewrite it for the running executable.
void repairStaleAgent()
{
    if (! isAgentInstalled())
        return;
    NSDictionary* existing = [NSDictionary dictionaryWithContentsOfFile: agentPath()];
    NSArray* arguments = existing[@"ProgramArguments"];
    NSString* executable = NSBundle.mainBundle.executablePath;
    if (executable != nil && ! [arguments.firstObject isEqual: executable])
        installAgent();
}

// ---------------------------------------------------------------- launch detection

bool hasLoginArgument() { return [NSProcessInfo.processInfo.arguments containsObject: LOGIN_ARGUMENT]; }

// Valid only while the 'oapp' event is handled, i.e. inside didFinishLaunching.
bool isLoginOpenEvent()
{
    NSAppleEventDescriptor* event = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
    return event.eventID == kAEOpenApplication
        && [event paramDescriptorForKeyword: keyAEPropData].enumCodeValue == keyAELaunchedAsLogInItem;
}
} // namespace

bool isEnabled() { return serviceState() == ServiceState::enabled || isAgentInstalled(); }

bool setEnabled (bool shouldStartAtLogin)
{
    const auto state = serviceState();
    if (state == ServiceState::awaitingApproval)
        openLoginItemsSettings(); // the user switched OscillaFX off there; let them see it

    if (! shouldStartAtLogin)
    {
        if (state == ServiceState::enabled || state == ServiceState::awaitingApproval)
            unregisterService();
        removeAgent();
        return ! isEnabled();
    }
    if (state == ServiceState::enabled || registerService())
    {
        removeAgent();
        return true;
    }
    return installAgent(); // service refused or pending approval: the LaunchAgent works meanwhile
}

void onLaunchFinished (std::function<void (bool)> callback)
{
    repairStaleAgent();
    if (NSRunningApplication.currentApplication.finishedLaunching)
        return callback (hasLoginArgument());

    __block id observer = nil;
    observer = [NSNotificationCenter.defaultCenter addObserverForName: NSApplicationDidFinishLaunchingNotification
                                                               object: nil
                                                                queue: nil
                                                           usingBlock: ^(NSNotification*) {
                                                               [NSNotificationCenter.defaultCenter removeObserver: observer];
                                                               callback (hasLoginArgument() || isLoginOpenEvent());
                                                           }];
}
} // namespace LoginItem
