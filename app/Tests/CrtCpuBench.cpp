// Times the CPU CRT renderer (the only one on Windows) on a busy scope trace at the default window
// size, for 100 % / 150 % / 200 % display scaling. Fails if a frame would not fit the 30 fps budget.
#include "ui/crt/CpuCrtRenderer.h"

#include <cmath>
#include <cstdio>

namespace
{
constexpr double FRAME_BUDGET_MS = 33.3; // CrtDisplay paces the CPU renderer at 30 fps
constexpr int FRAMES = 120;

Crt::Scene scopeScene (int frame)
{
    Crt::Scene scene;
    scene.bounds = { 0.0f, 0.0f, 456.0f, 342.0f };   // Theme::Design::glass at design scale
    scene.graticule = scene.bounds.reduced (24.0f);
    scene.cornerRadius = 26.0f;
    scene.dt = 1.0f / 30.0f;
    scene.time = frame * scene.dt;
    const auto area = scene.graticule;
    juce::Point<float> previous;
    for (int i = 0; i <= 1024; ++i)   // a two-tone waveform, as the scope draws it
    {
        const auto x = (float) i / 1024.0f;
        const auto phase = (float) frame * 0.21f;
        const auto y = 0.5f + 0.3f * std::sin (x * 25.0f + phase) + 0.1f * std::sin (x * 143.0f - phase);
        const juce::Point<float> point (area.getX() + x * area.getWidth(), area.getY() + y * area.getHeight());
        if (i > 0)
            scene.beams.push_back ({ previous, point, 1.0f, 0.85f });
        previous = point;
    }
    return scene;
}

double millisecondsPerFrame (float pixelScale)
{
    CpuCrtRenderer renderer;
    juce::Image target (juce::Image::RGB, (int) std::ceil (456 * pixelScale), (int) std::ceil (342 * pixelScale), true);
    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (int frame = 0; frame < FRAMES; ++frame)
    {
        const auto scene = scopeScene (frame);
        renderer.render (scene);
        juce::Graphics g (target);
        g.addTransform (juce::AffineTransform::scale (pixelScale));
        renderer.paint (g, scene);
    }
    return (juce::Time::getMillisecondCounterHiRes() - start) / FRAMES;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    int failures = 0;
    for (const float scale : { 1.0f, 1.5f, 2.0f })
    {
        const auto ms = millisecondsPerFrame (scale);
        const auto ok = ms < FRAME_BUDGET_MS;
        std::printf ("[%s] CPU CRT at %.0f %%: %.2f ms per frame (budget %.1f ms)\n", ok ? "PASS" : "FAIL", scale * 100.0f, ms, FRAME_BUDGET_MS);
        failures += ok ? 0 : 1;
    }
    std::printf ("%s\n", failures == 0 ? "ALL PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
