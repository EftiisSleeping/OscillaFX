#pragma once

#include "ui/crt/CrtScene.h"
#include <juce_gui_extra/juce_gui_extra.h>

// GPU CRT: a CAMetalLayer-backed NSView hosted in the JUCE hierarchy. Each frame:
//   1. picture pass   phosphor(n) = decay(phosphor(n-1)) + overlay/gallery refresh   (R16F, HDR)
//   2. beam pass      additive gaussian line quads (erf-integrated, so polylines join seamlessly)
//   3. bloom          dual-filter pyramid of the phosphor energy at 1/4 and 1/8 size (tight halo)
//   4. composite      slight barrel lens, tone map (teal skirt -> near-white cyan core + deep teal halo),
//                     faint device-aligned scanlines, analytic crisp graticule, vignette, glass, crisp
//                     hint text, rounded tube mask -> drawable
// The view ignores mouse clicks (they reach the owning JUCE component) and nothing JUCE paints can
// appear above it, so everything on the tube face is rendered here.
class MetalCrtView : public juce::NSViewComponent
{
public:
    // nullptr (and a log line) when Metal or the shaders are unavailable; callers fall back to CPU.
    static std::unique_ptr<MetalCrtView> create();
    ~MetalCrtView() override;

    void render (const Crt::Scene&);   // drops the frame rather than block when the GPU is behind
    bool hasFailed() const;             // device removed or repeated GPU errors: switch to the CPU renderer
    juce::Point<int> drawableSize() const;   // device pixels; overlay images must match it exactly
    bool isVisibleOnScreen() const;   // false when the window is hidden, minimised or fully occluded

    // The last rendered picture read back from the GPU (CAMetalLayer content cannot be captured by
    // createComponentSnapshot). Device-pixel sized; invalid if nothing has been rendered yet.
    juce::Image capture();

    static bool prefersReducedMotion();

private:
    struct Impl;
    explicit MetalCrtView (std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetalCrtView)
};
