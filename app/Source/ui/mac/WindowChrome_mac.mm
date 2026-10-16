#include "ui/WindowChrome.h"

#import <Cocoa/Cocoa.h>

namespace WindowChrome
{
void useFullSizeContent (juce::ComponentPeer& peer)
{
    auto* view = (NSView*) peer.getNativeHandle();
    NSWindow* window = [view window];
    if (window == nil)
        return;

    window.styleMask |= NSWindowStyleMaskFullSizeContentView;
    window.titlebarAppearsTransparent = YES;
    window.titleVisibility = NSWindowTitleHidden;
    window.movableByWindowBackground = NO;   // the panel drags the window itself (MainPanel), so controls keep their clicks
}

void detentHaptic()
{
    [[NSHapticFeedbackManager defaultPerformer] performFeedbackPattern: NSHapticFeedbackPatternAlignment
                                                       performanceTime: NSHapticFeedbackPerformanceTimeDefault];
}
}
