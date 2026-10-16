#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The GPU CRT renderer CrtDisplay prefers over CpuCrtRenderer. macOS: Metal (ui/crt/mac/MetalCrtView).
// Other platforms have none yet: create() returns nullptr and CrtDisplay renders on the CPU.
// A new GPU renderer is a juce::Component providing exactly this interface.
#if JUCE_MAC
 #include "ui/crt/mac/MetalCrtView.h"
using GpuCrtView = MetalCrtView;
#else
 #include "ui/crt/CrtScene.h"

class GpuCrtView : public juce::Component
{
public:
    static std::unique_ptr<GpuCrtView> create() { return nullptr; }
    static bool prefersReducedMotion() { return false; }

    void render (const Crt::Scene&) {}
    bool hasFailed() const { return true; }
    juce::Point<int> drawableSize() const { return {}; }
    bool isVisibleOnScreen() const { return false; }
    juce::Image capture() { return {}; }
};
#endif
