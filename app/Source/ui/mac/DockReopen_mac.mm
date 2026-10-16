#include "ui/DockReopen.h"

#if JUCE_MAC || defined (__APPLE__)
 #import <Cocoa/Cocoa.h>
 #include <objc/runtime.h>

namespace DockReopen
{
namespace
{
    std::function<void()>& handler()
    {
        static std::function<void()> instance;
        return instance;
    }

    BOOL shouldHandleReopen (id, SEL, NSApplication*, BOOL)
    {
        if (handler() != nullptr)
            handler()();
        return NO; // we have shown the window ourselves
    }

    void installOnDelegate()
    {
        static bool isInstalled = false;
        id delegate = [NSApp delegate];
        if (isInstalled || delegate == nil)
            return;

        const auto selector = @selector (applicationShouldHandleReopen:hasVisibleWindows:);
        if (class_addMethod ([delegate class], selector, (IMP) shouldHandleReopen, "c@:@c"))
            isInstalled = true;
    }
}

void setHandler (std::function<void()> onReopen)
{
    handler() = std::move (onReopen);
    installOnDelegate();
}
}
#endif
