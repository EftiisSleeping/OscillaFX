#include "ui/CrtDisplay.h"
#include "ui/Brand.h"
#include "ui/Theme.h"
#include "ui/crt/CrtLens.h"
#include "ui/crt/CrtPalette.h"
#include "ui/crt/CrtTraces.h"
#include "ui/crt/GpuCrtView.h"

namespace
{
    constexpr int columns = 10;
    constexpr int spectrumBars = 16;
    constexpr float gratMarginX = 25.0f;
    constexpr float gratMarginY = 26.0f;
    constexpr float eqRangeDb = 12.0f;
    constexpr float eqStepDb = 0.5f;
    constexpr float warmUpSeconds = 0.9f;
    constexpr float collapseSeconds = 0.45f;
    constexpr float afterglowSeconds = 0.35f;
    constexpr float degaussSeconds = 0.7f;
    constexpr float burnSeconds = 1.1f;
    constexpr float splashHoldSeconds = 0.5f;    // butterfly at full strength after power-on ...
    constexpr float splashFadeSeconds = 1.2f;    // ... then fades over this long
    constexpr float splashIdleGain = 0.07f;      // dim, static butterfly on a silent scope
    constexpr int splashPictureId = 0x0b7e0001;
    constexpr float silenceThreshold = 1.0e-4f;
    constexpr int waveWindow = 1024;
    constexpr int xyWindow = 1024;
    constexpr int triggerSearch = 2048;
    constexpr int maxTriggerCandidates = 24;
    constexpr float triggerSmoothing = 0.06f;
    constexpr float maxFrameSeconds = 0.1f;
    constexpr float settleSeconds = 2.0f;        // > a few persistence time constants
    constexpr double gpuFrameSeconds = 1.0 / 60.0;
    constexpr double cpuFrameSeconds = 1.0 / 30.0;
    constexpr float scaleIllumination = 0.22f;   // graticule visibility with the beam off
    constexpr float labelLevel = 0.85f;          // phosphor energy of overlay text
    constexpr float hintFadeSeconds = 0.14f;
    constexpr float hintTextHeight = 11.0f;
    constexpr float hintBandHeight = 16.0f;
    constexpr float waveEnergy = 1.0f;           // peak energy one fresh sweep of the wave deposits
    constexpr float xyEnergy = 0.32f;            // X-Y path: thinner, its loops overlap
    constexpr float idleEnergy = 1.1f;           // flat baseline of a silent scope (static: this is its steady state)
    constexpr float waveBlendSeconds = 0.07f;    // successive sweeps are blended: one line that morphs, not several ghosts
    constexpr float xyBudgetPixels = 900.0f;     // path length that gets the full xyEnergy; longer paths dim
    constexpr int minSlideshowSeconds = 2, maxSlideshowSeconds = 120;

    constexpr float eqFineStepDb = 0.1f;

    float bloomFor (CrtDisplay::Mode mode)
    {
        switch (mode)
        {
            case CrtDisplay::Mode::wave:    return 0.65f;
            case CrtDisplay::Mode::xy:      return 0.6f;
            case CrtDisplay::Mode::gallery: return 0.75f;
            case CrtDisplay::Mode::spectrum: break;
        }
        return 0.6f;
    }
    float square (float x) { return x * x; }
}

CrtDisplay::CrtDisplay (AudioEngine& e)
    : engine (e),
      vblank (this, [this] (double t) { tick (t); }),
      left ((size_t) scopeCapacity),
      right ((size_t) scopeCapacity),
      mono ((size_t) scopeCapacity),
      trigger ((size_t) scopeCapacity)
{
    setTitle ("Oscilloscope display");
    setOpaque (true);
    reducedMotion = GpuCrtView::prefersReducedMotion();
    gallery.rescan();

    for (int i = 0; i < columns; ++i)
    {
        auto& accessor = bandAccessors[(size_t) i];
        accessor = std::make_unique<EqBandAccessor> (EqBandAccessor::Range { -eqRangeDb, eqRangeDb, eqStepDb },
                                                     [this, i] { return eqGains[(size_t) i]; },
                                                     [this, i] (float db) { setBandGain (i, db); });
        addChildComponent (*accessor);
    }
    setEqFrequencies (eqFrequencies);
}

CrtDisplay::~CrtDisplay() = default;

// Created on first show rather than at construction: an app started hidden in the menu bar never
// touches the GPU.
void CrtDisplay::ensureRenderer()
{
    if (hasTriedGpu || ! isShowing())
        return;
    hasTriedGpu = true;
    gpu = GpuCrtView::create();
    if (gpu == nullptr)
        return;
    addAndMakeVisible (*gpu, 0);
    gpu->setBounds (getLocalBounds());
    wake();
}

void CrtDisplay::fallBackToCpu()
{
    juce::Logger::writeToLog ("CRT (GPU): device lost or failing, switching to the CPU renderer");
    removeChildComponent (gpu.get());
    gpu.reset();
    renderedPixels = {};
    wake();
    repaint();
}

void CrtDisplay::wake()
{
    quietSeconds = 0.0f;
}

void CrtDisplay::setPowered (bool on)
{
    if (on == isPowered)
        return;
    wake();
    isPowered = on;
    poweredSeconds = 0.0f;
}

void CrtDisplay::setMode (Mode m)
{
    if (m == mode)
        return;
    wake();
    mode = m;
    waveShown.clear();
    if (mode == Mode::gallery)
    {
        gallery.rescan();
        galleryMotion.nextTear = clock + 2.5;   // the picture "tunes in" with a brief tracking glitch
        slideshowNext = clock + slideshowSeconds;
    }
    updateCursor();
}

void CrtDisplay::updateCursor()
{
    if (isEqEditing)
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else
        setMouseCursor (mode == Mode::gallery ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void CrtDisplay::setEqEditing (bool editing)
{
    wake();
    isEqEditing = editing;
    draggedBand = -1;
    pendingGains.reset();
    setWantsKeyboardFocus (editing);
    if (! editing && hasKeyboardFocus (false))
        giveAwayKeyboardFocus();
    for (auto& accessor : bandAccessors)
        accessor->setVisible (editing);
    updateCursor();
    setDescription (editing ? "Drag the ten points vertically to set the equaliser bands" : "Audio visualiser");
}

void CrtDisplay::setEqGains (const EqGains& gains)
{
    if (draggedBand >= 0)
    {
        pendingGains = gains;   // don't fight the drag; applied on release
        return;
    }
    if (gains == eqGains)
        return;
    eqGains = gains;
    for (auto& accessor : bandAccessors)
        accessor->gainChanged();
    wake();
}

void CrtDisplay::setEqFrequencies (const std::array<float, DspSettings::numEqBands>& hz)
{
    eqFrequencies = hz;
    for (int i = 0; i < columns; ++i)
    {
        const auto f = hz[(size_t) i];
        const auto spoken = f < 1000.0f ? juce::String (juce::roundToInt (f)) + " Hz"
                                        : juce::String (f / 1000.0f, f < 10000.0f ? 1 : 0) + " kHz";
        bandAccessors[(size_t) i]->setTitle ("EQ " + spoken);
    }
    wake();
}

void CrtDisplay::setMessage (const juce::String& m)
{
    if (m.toUpperCase() != message)
        wake();
    message = m.toUpperCase();
}

void CrtDisplay::setGalleryIndex (int index)
{
    const auto count = gallery.size();
    index = count > 0 ? ((index % count) + count) % count : 0;
    if (index == galleryMotion.index)
        return;
    galleryMotion.index = index;   // the burn starts once the new picture is decoded and shown
    wake();
}

int CrtDisplay::galleryCount() const
{
    return gallery.size();
}

void CrtDisplay::setHoverHint (const juce::String& text)
{
    const auto line = text.replaceCharacters ("\r\n", "  ").trim();
    if (line == hintWanted)
        return;
    hintWanted = line;
    wake();
}

void CrtDisplay::setGallerySlideshow (bool enabled, int seconds)
{
    slideshowOn = enabled;
    slideshowSeconds = (double) juce::jlimit (minSlideshowSeconds, maxSlideshowSeconds, seconds);
    slideshowNext = clock + slideshowSeconds;
    wake();
}

void CrtDisplay::reloadGallery()
{
    gallery.rescan (true);
    const auto count = gallery.size();
    galleryMotion.index = count > 0 ? juce::jlimit (0, count - 1, galleryMotion.index) : 0;
    slideshowNext = clock + slideshowSeconds;
    wake();
}

void CrtDisplay::advanceSlideshow()
{
    slideshowNext = clock + slideshowSeconds;
    gallery.rescan();
    if (gallery.size() < 2)
        return;
    const auto next = (galleryMotion.index + 1) % gallery.size();
    setGalleryIndex (next);
    if (onGalleryIndexChanged != nullptr)
        onGalleryIndexChanged (next);
}

void CrtDisplay::resized()
{
    const auto local = getLocalBounds().toFloat();
    graticule = local.reduced (gratMarginX, gratMarginY);
    scene.bounds = local;
    scene.graticule = graticule;
    scene.cornerRadius = Theme::Design::glassCorner;
    renderedPixels = {};
    if (gpu != nullptr)
        gpu->setBounds (getLocalBounds());
    layOutBandAccessors();
    wake();
}

void CrtDisplay::layOutBandAccessors()
{
    const auto colW = graticule.getWidth() / columns;
    for (int i = 0; i < columns; ++i)
        bandAccessors[(size_t) i]->setBounds (graticule.withX (graticule.getX() + colW * (float) i).withWidth (colW).toNearestInt());
}

void CrtDisplay::setRendersWhenOccluded (bool shouldRender)
{
    rendersWhenOccluded = shouldRender;
    wake();
}

juce::Image CrtDisplay::renderedScreen()
{
    return gpu != nullptr ? gpu->capture() : cpu.snapshot();
}

//==============================================================================
void CrtDisplay::tick (double now)
{
    ensureRenderer();
    if (gpu != nullptr && gpu->hasFailed())
        fallBackToCpu();

    // Paced to ~60 fps on the GPU (also on 120 Hz displays); 30 fps for the slow-moving gallery
    // and on the CPU fallback.
    const auto isSlowPicture = mode == Mode::gallery && ! isEqEditing;
    const auto interval = gpu != nullptr && ! isSlowPicture ? gpuFrameSeconds : cpuFrameSeconds;
    if (lastFrame > 0.0 && now - lastFrame < interval * 0.8)
        return;
    const auto dt = lastFrame > 0.0 ? juce::jlimit (0.0f, maxFrameSeconds, (float) (now - lastFrame)) : 0.0f;
    lastFrame = now;
    if (! isOnScreen())
        return;

    // the window was rescaled (the panel scales by transform, so resized() is not called)
    if (gpu != nullptr && gpu->drawableSize() != renderedPixels)
        wake();

    clock += dt;
    advancePower (dt);
    if (isPowered && beam > 0.0f)
        readSignal (dt);
    if (slideshowOn && mode == Mode::gallery && isPowered && ! isEqEditing && clock >= slideshowNext)
        advanceSlideshow();

    // Nothing moves on screen: once the phosphor has settled, stop rendering until something changes.
    quietSeconds = isStatic() ? quietSeconds + dt : 0.0f;
    if (quietSeconds > settleSeconds)
        return;

    buildScene (dt);
    if (gpu != nullptr)
    {
        gpu->render (scene);
        return;
    }
    cpu.render (scene);
    repaint();
}

bool CrtDisplay::isOnScreen() const
{
    return isShowing() && (gpu == nullptr || rendersWhenOccluded || gpu->isVisibleOnScreen());
}

bool CrtDisplay::isStatic() const
{
    if (! isPowered)
        return beam <= 0.0f && afterglow < 0.01f && hintLevel <= 0.0f;   // the hint fades out with the beam
    if (beam < 1.0f || poweredSeconds < juce::jmax (degaussSeconds, splashHoldSeconds + splashFadeSeconds))
        return false;
    if (! isHintSettled())
        return false;
    if (isEqEditing)
        return draggedBand < 0;
    if (mode == Mode::gallery)
        return gallery.size() == 0 || (reducedMotion && galleryMotion.burn <= 0.0f && isSilent && shownPictureId != 0);
    return isSilent;
}

void CrtDisplay::advancePower (float dt)
{
    const auto step = isPowered ? dt / warmUpSeconds : -dt / collapseSeconds;
    beam = juce::jlimit (0.0f, 1.0f, beam + step);
    poweredSeconds = isPowered ? poweredSeconds + dt : 0.0f;

    // power-off: the squashed line turns into a dot that lingers and fades
    if (isPowered)
        afterglow = 0.0f;
    else
        afterglow = beam > 0.0f ? juce::jmax (0.0f, 1.0f - beam * 2.0f) : afterglow * std::exp (-dt / afterglowSeconds);
}

void CrtDisplay::readSignal (float dt)
{
    std::array<float, DspSettings::numEqBands> raw {};
    engine.getSpectrum (raw.data());

    const auto release = 1.0f - std::exp (-dt / 0.18f);
    const auto peakFall = dt * 0.35f;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const auto target = (raw[i] >= 0.0f && raw[i] <= 1.0f) ? raw[i] : 0.0f; // engine reports 0..1, junk otherwise
        bands[i] += (target - bands[i]) * (target > bands[i] ? 0.6f : release);
        peaks[i] = juce::jmax (bands[i], peaks[i] - peakFall);
    }

    const auto bass = 0.5f * (bands[0] + bands[1]);
    bassFast += (bass - bassFast) * (1.0f - std::exp (-dt / 0.04f));
    bassSlow += (bass - bassSlow) * (1.0f - std::exp (-dt / 0.6f));

    sampleCount = juce::jlimit (0, scopeCapacity, engine.getScopeSamples (left.data(), right.data(), scopeCapacity));
    auto peak = 0.0f;
    for (int i = 0; i < sampleCount; ++i)
        peak = juce::jmax (peak, std::abs (left[(size_t) i]), std::abs (right[(size_t) i]));
    isSilent = peak < silenceThreshold;
}

//==============================================================================
float CrtDisplay::persistence() const
{
    if (! isPowered)
        return 0.6f;
    if (isEqEditing)
        return 0.05f;
    switch (mode)
    {
        case Mode::spectrum: return 0.06f;
        case Mode::wave:     return 0.012f;  // very short: the next sweep must not sit on top of a ghost of the last
        case Mode::xy:       return 0.09f;
        case Mode::gallery:  return 0.10f + 0.45f * galleryMotion.burn;
    }
    return 0.07f;
}

void CrtDisplay::buildScene (float dt)
{
    scene.beams.clear();
    scene.gallery = {};
    scene.time = clock;
    scene.dt = dt;
    scene.persistence = persistence();
    scene.pictureResponse = 0.08f + 0.3f * galleryMotion.burn;
    scene.saturation = Crt::palette.saturation;
    scene.brightness = isPowered ? beam * beam : 1.0f;
    scene.bloom = bloomFor (isEqEditing ? Mode::spectrum : mode);
    scene.collapseX = isPowered ? 1.0f : juce::jlimit (0.002f, 1.0f, beam * 2.0f);
    scene.collapseY = isPowered ? 1.0f : juce::jlimit (0.002f, 1.0f, (beam - 0.5f) * 2.0f);
    scene.afterglow = afterglow;
    scene.degauss = isPowered ? square (juce::jmax (0.0f, 1.0f - poweredSeconds / degaussSeconds)) : 0.0f;
    scene.scaleIllumination = scaleIllumination + (1.0f - scaleIllumination) * beam;
    scene.faceGlow = beam;
    scene.reducedMotion = reducedMotion;

    if (isPowered && beam > 0.0f)
    {
        if (isEqEditing)
            addEq();
        else if (mode == Mode::spectrum)
            addSpectrum();
        else if (mode == Mode::wave)
            addWave();
        else if (mode == Mode::xy)
            addXY();
        else
            updateGallery (dt);
        if (mode != Mode::gallery || isEqEditing)
            updateSplash();
    }
    updateOverlay();
    updateHint (dt);
    renderedPixels = gpu != nullptr ? gpu->drawableSize() : juce::Point<int>();
}

void CrtDisplay::addSpectrum()
{
    Crt::Traces::spectrum (scene.beams, graticule.reduced (6.0f, 2.0f), bands.data(), peaks.data(), (int) bands.size(), spectrumBars);
}

// Beam density that makes one fresh sweep peak at `energy`: the renderer deposits the fraction of
// its density that the phosphor lost this frame, so a trace that moves every frame (wave, X-Y) is
// scaled back up, while one that stays put builds up to the phosphor's saturation.
float CrtDisplay::freshDensity (float energy) const
{
    const auto lost = 1.0f - std::exp (-juce::jmax (scene.dt, 1.0f / 120.0f) / juce::jmax (0.001f, scene.persistence));
    return energy / juce::jmax (0.04f, lost);
}

void CrtDisplay::addWave()
{
    const auto window = juce::jmin (waveWindow, sampleCount);
    const auto sigma = Crt::palette.traceSigma;
    if (isSilent || window < 64)
    {
        // an idle scope still draws its flat baseline
        waveShown.clear();
        Crt::Traces::line (scene.beams, { { graticule.getX(), graticule.getCentreY() }, { graticule.getRight(), graticule.getCentreY() } },
                           idleEnergy, sigma);
        return;
    }

    auto slow1 = 0.0f, slow2 = 0.0f;
    for (int i = 0; i < sampleCount; ++i)
    {
        mono[(size_t) i] = 0.5f * (left[(size_t) i] + right[(size_t) i]);
        slow1 += (mono[(size_t) i] - slow1) * triggerSmoothing;   // two one-poles: ~-12 dB/octave above ~450 Hz
        slow2 += (slow1 - slow2) * triggerSmoothing;
        trigger[(size_t) i] = slow2;
    }
    const auto start = findTrigger (window);

    // One clean line: never the raw samples (see Crt::Traces::waveLevels), normalised so the trace
    // fills about 60% of the screen height whatever the volume.
    auto levels = Crt::Traces::waveLevels (mono.data() + start, window);
    if (waveShown.size() == levels.size())
    {
        const auto blend = 1.0f - std::exp (-scene.dt / waveBlendSeconds);
        for (size_t i = 0; i < levels.size(); ++i)
            levels[i] = waveShown[i] + (levels[i] - waveShown[i]) * blend;
    }
    waveShown = levels;
    waveGain.update (Crt::Traces::peakOf (levels), scene.dt);
    const auto points = Crt::Traces::waveLine (levels, graticule, waveGain.gain());
    Crt::Traces::line (scene.beams, points, freshDensity (waveEnergy), sigma);
}

// Rising zero crossing (with hysteresis) whose following window best matches the previous sweep,
// so complex material locks like a scope with holdoff instead of hopping between crossings. Looks
// only at the bass and low mids (a low-passed copy): hi-hat noise must not decide where the sweep starts.
int CrtDisplay::findTrigger (int window)
{
    auto peak = 0.0f;
    for (int i = 0; i < sampleCount; ++i)
        peak = juce::jmax (peak, std::abs (trigger[(size_t) i]));
    const auto hysteresis = peak * 0.1f;
    const auto stride = window / triggerProbePoints;

    auto best = sampleCount - window;
    auto bestScore = std::numeric_limits<float>::max();
    auto candidates = 0;
    for (int i = sampleCount - window; i > juce::jmax (8, sampleCount - window - triggerSearch) && candidates < maxTriggerCandidates; --i)
    {
        if (! (trigger[(size_t) (i - 1)] < 0.0f && trigger[(size_t) i] >= 0.0f && trigger[(size_t) (i - 8)] < -hysteresis))
            continue;

        ++candidates;
        auto score = 0.0f;
        for (int k = 0; k < triggerProbePoints; ++k)
            score += std::abs (trigger[(size_t) (i + k * stride)] - lastSweep[(size_t) k]);
        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }

    for (int k = 0; k < triggerProbePoints; ++k)
        lastSweep[(size_t) k] = trigger[(size_t) (best + k * stride)];
    return best;
}

void CrtDisplay::addXY()
{
    const auto c = graticule.getCentre();
    const auto window = juce::jmin (xyWindow, sampleCount);
    const auto sigma = Crt::palette.traceSigma;
    if (isSilent || window < 64)
    {
        Crt::Traces::spot (scene.beams, c, 0.9f, sigma * 1.6f);
        return;
    }

    // Slow auto-gain: typical music fills about 60% of the plot; mono draws a thin diagonal, wide
    // stereo a loose ellipse. A square plot area keeps the diagonal at 45 degrees.
    const auto first = sampleCount - window;
    const auto levels = Crt::Traces::xyLevels (left.data() + first, right.data() + first, window);
    xyGain.update (juce::jmax (Crt::Traces::peakOf (levels.x), Crt::Traces::peakOf (levels.y)), scene.dt);
    const auto half = juce::jmin (graticule.getWidth(), graticule.getHeight()) * 0.5f * 0.98f;
    const auto points = Crt::Traces::xyLine (levels, c, half, xyGain.gain());

    // Limit what one frame can deposit: a long, tangled path is drawn dimmer instead of piling up
    // into a white blob (the phosphor also saturates, see Crt::Palette::saturation).
    const auto budget = juce::jlimit (0.35f, 1.0f, xyBudgetPixels / juce::jmax (1.0f, Crt::Traces::pathLength (points)));
    Crt::Traces::line (scene.beams, points, freshDensity (xyEnergy * budget), sigma);
}

void CrtDisplay::addEq()
{
    const auto colW = graticule.getWidth() / columns;
    Crt::Traces::Points points { { graticule.getX(), yForGain (eqGains.front()) } };
    for (int i = 0; i < columns; ++i)
        points.push_back ({ graticule.getX() + colW * ((float) i + 0.5f), yForGain (eqGains[(size_t) i]) });
    points.push_back ({ graticule.getRight(), yForGain (eqGains.back()) });

    Crt::Traces::line (scene.beams, Crt::Traces::smooth (points, 3.0f), 0.85f, Crt::palette.traceSigma);
    for (int i = 0; i < columns; ++i)
    {
        const auto isActive = i == highlightedBand();
        Crt::Traces::spot (scene.beams, points[(size_t) i + 1], isActive ? 1.25f : 0.95f, isActive ? 3.0f : 2.2f);
    }
}

void CrtDisplay::updateGallery (float dt)
{
    auto& m = galleryMotion;
    m.burn = juce::jmax (0.0f, m.burn - dt / burnSeconds);
    const auto* picture = gallery.get (m.index);
    if (picture == nullptr)
        return;   // still decoding: the previous picture fades meanwhile
    if (picture->id != shownPictureId)
    {
        m.burn = shownPictureId != 0 ? 1.0f : 0.0f;   // phosphor burn when the picture changes
        shownPictureId = picture->id;
    }

    auto area = scene.bounds.reduced (scene.bounds.getWidth() * 0.05f, scene.bounds.getHeight() * 0.06f);
    const auto aspect = (float) picture->width / (float) picture->height;
    area = area.getAspectRatio() > aspect ? area.withSizeKeepingCentre (area.getHeight() * aspect, area.getHeight())
                                          : area.withSizeKeepingCentre (area.getWidth(), area.getWidth() / aspect);

    const auto t = (float) clock;
    const auto motion = reducedMotion ? 0.0f : 1.0f;
    const auto breathe = 1.0f + 0.06f * motion * std::sin (t * juce::MathConstants<float>::twoPi / 5.5f);
    const auto pulse = juce::jlimit (0.0f, 1.0f, (bassFast - bassSlow) * 3.0f + bassFast * 0.25f);

    auto& g = scene.gallery;
    g.current = picture;
    g.area = area;
    g.gain = 0.8f * breathe * (1.0f + 0.15f * pulse) * (1.0f + 0.6f * m.burn) * beam * beam;
    g.drift = juce::Point<float> (std::sin (t * 0.21f) * 3.5f, std::sin (t * 0.15f + 1.3f) * 2.5f) * motion;
    scene.bloom *= 1.0f + 0.2f * pulse;

    // occasional tracking glitch: a horizontal tear through a band of rows
    if (! reducedMotion && clock >= m.nextTear)
    {
        m.tearStart = clock;
        m.tearDuration = 0.22f + 0.3f * random.nextFloat();
        m.tearY = 0.15f + 0.7f * random.nextFloat();
        m.tearHeight = 0.04f + 0.08f * random.nextFloat();
        m.tearSeed = random.nextFloat() * 100.0f;
        m.nextTear = clock + 5.0 + 9.0 * random.nextDouble();
    }
    const auto phase = (float) ((clock - m.tearStart) / m.tearDuration);
    g.tear = phase >= 0.0f && phase <= 1.0f ? std::sin (juce::MathConstants<float>::pi * phase) : 0.0f;
    g.tearY = m.tearY;
    g.tearHeight = m.tearHeight;
    g.tearSeed = m.tearSeed;
}

// The brand mark reuses the gallery picture pass (auto-levelled luminance, bloom, dither look).
void CrtDisplay::updateSplash()
{
    if (! splashImage.isValid())
    {
        const auto& mark = Brand::mark();
        if (! mark.isValid())
            return;
        splashImage.width = mark.getWidth();
        splashImage.height = mark.getHeight();
        splashImage.id = splashPictureId;
        splashImage.pixels.resize ((size_t) (splashImage.width * splashImage.height));
        const juce::Image::BitmapData pixels (mark, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < splashImage.height; ++y)
            for (int x = 0; x < splashImage.width; ++x)
                splashImage.pixels[(size_t) (y * splashImage.width + x)] = pixels.getPixelColour (x, y).getAlpha();
    }

    const auto fade = 1.0f - juce::jlimit (0.0f, 1.0f, (poweredSeconds - splashHoldSeconds) / splashFadeSeconds);
    const auto warmUp = fade * fade * (3.0f - 2.0f * fade);
    const auto idle = isSilent && message.isEmpty() && ! isEqEditing ? splashIdleGain : 0.0f;
    const auto gain = juce::jmax (1.7f * warmUp, idle);
    if (gain <= 0.001f)
        return;

    auto area = scene.bounds.reduced (scene.bounds.getWidth() * 0.05f, scene.bounds.getHeight() * 0.06f);
    area = area.withSizeKeepingCentre (area.getHeight(), area.getHeight());
    auto& g = scene.gallery;
    g.current = &splashImage;
    g.area = area;
    g.gain = gain * beam;
}

//==============================================================================
std::vector<Crt::Artwork::Label> CrtDisplay::currentLabels() const
{
    std::vector<Crt::Artwork::Label> labels;
    if (message.isNotEmpty())
        labels.push_back ({ message, graticule.reduced (30.0f, 0.0f).withTrimmedTop (graticule.getHeight() * 0.18f).withHeight (graticule.getHeight() * 0.3f),
                            14.0f, 0.9f });

    if (isEqEditing)
    {
        const auto colW = graticule.getWidth() / columns;
        for (int i = 0; i < columns; ++i)
            labels.push_back ({ bandLabel (i), { graticule.getX() + colW * (float) i, graticule.getBottom() + 3.0f, colW, 14.0f }, 10.0f, 0.5f });
        for (auto db : { eqRangeDb, 0.0f, -eqRangeDb })
            labels.push_back ({ db > 0.0f ? "+12" : (db < 0.0f ? "-12" : "0"), { 2.0f, yForGain (db) - 7.0f, gratMarginX - 4.0f, 14.0f },
                                9.0f, 0.45f, juce::Justification::centredRight });
        if (const auto band = highlightedBand(); band >= 0)
        {
            const auto gain = eqGains[(size_t) band];
            const auto readout = bandLabel (band) + " HZ  " + (gain > 0.0f ? "+" : "") + juce::String (gain, 1) + " DB";
            labels.push_back ({ readout, graticule.withHeight (24.0f).translated (0.0f, 6.0f), 13.0f, 1.0f });
        }
    }
    else if (mode == Mode::gallery && gallery.size() == 0)
    {
        labels.push_back ({ juce::String (juce::CharPointer_UTF8 ("NO IMAGES \xe2\x80\x94 ADD TO GALLERY FOLDER")),
                            graticule.withSizeKeepingCentre (graticule.getWidth(), 40.0f), 13.0f, 0.9f });
    }
    return labels;
}

juce::Point<int> CrtDisplay::overlayPixels() const
{
    if (gpu != nullptr)
        return gpu->drawableSize();
    return { getWidth() * 2, getHeight() * 2 };
}

void CrtDisplay::updateOverlay()
{
    auto labels = currentLabels();
    const auto pixels = overlayPixels();
    if (labels != overlayLabels || pixels != overlaySize)
    {
        overlayImage = labels.empty() || pixels.x <= 0 ? juce::Image() : Crt::Artwork::labels (labels, scene.bounds, pixels);
        overlayLabels = std::move (labels);
        overlaySize = pixels;
        ++scene.overlay.version;
    }
    scene.overlay.image = overlayImage.isValid() ? &overlayImage : nullptr;
    scene.overlay.level = isPowered ? labelLevel * beam * beam : 0.0f;
}

bool CrtDisplay::isHintSettled() const
{
    if (hintShown != hintWanted)
        return false;
    return hintShown.isEmpty() ? hintLevel <= 0.0f : (isPowered ? hintLevel >= 1.0f : hintLevel <= 0.0f);
}

// The help line fades out, swaps its text while invisible, then fades in: one line, never two
// overlapping. It is drawn as a crisp mask over the glass (no phosphor persistence, no bloom).
void CrtDisplay::updateHint (float dt)
{
    const auto wantsShown = isPowered && hintShown == hintWanted && hintShown.isNotEmpty();
    const auto step = dt / hintFadeSeconds;
    hintLevel = wantsShown ? juce::jmin (1.0f, hintLevel + step) : juce::jmax (0.0f, hintLevel - step);

    if (hintLevel <= 0.0f && hintShown != hintWanted)
        hintShown = hintWanted;

    const auto pixels = overlayPixels();
    const auto onTop = isEqEditing;   // the EQ's frequency labels use the band below the grid
    if (hintShown.isEmpty() || pixels.x <= 0)
    {
        hintImage = {};
        hintSize = {};
        scene.hint.image = nullptr;
        scene.hint.level = 0.0f;
        return;
    }

    if (pixels != hintSize || onTop != hintOnTop || ! hintImage.isValid() || hintRenderedText != hintShown)
    {
        const auto band = onTop ? juce::Rectangle<float> (graticule.getX(), 5.0f, graticule.getWidth(), hintBandHeight)
                                : juce::Rectangle<float> (graticule.getX(), graticule.getBottom() + 4.0f, graticule.getWidth(), hintBandHeight);
        hintImage = Crt::Artwork::labels ({ { hintShown, band, hintTextHeight, 1.0f, juce::Justification::centred, false, true } },
                                          scene.bounds, pixels);
        hintSize = pixels;
        hintOnTop = onTop;
        hintRenderedText = hintShown;
        ++scene.hint.version;
    }
    scene.hint.image = &hintImage;
    scene.hint.level = hintLevel * beam * beam;
}

//==============================================================================
void CrtDisplay::paint (juce::Graphics& g)
{
    if (gpu != nullptr)
    {
        g.fillAll (juce::Colours::black);   // covered by the GPU view
        return;
    }
    cpu.paint (g, scene);
}

//==============================================================================
juce::String CrtDisplay::bandLabel (int band) const
{
    const auto f = eqFrequencies[(size_t) band];
    if (f < 1000.0f)
        return juce::String (juce::roundToInt (f));
    return juce::String (f / 1000.0f, f < 10000.0f ? 1 : 0) + "K";
}

int CrtDisplay::highlightedBand() const
{
    if (draggedBand >= 0)
        return draggedBand;
    return hasKeyboardFocus (false) ? keyboardBand : -1;
}

juce::Point<float> CrtDisplay::toPhosphor (juce::Point<float> screenPosition) const
{
    // The GPU tube face is curved; map through the same lens as the shader (exact, see CrtLens.h).
    return gpu != nullptr ? Crt::Lens::toPhosphor (screenPosition, getLocalBounds().toFloat()) : screenPosition;
}

int CrtDisplay::bandAt (float x) const
{
    return juce::jlimit (0, columns - 1, (int) ((x - graticule.getX()) / (graticule.getWidth() / columns)));
}

float CrtDisplay::gainAt (float y) const
{
    const auto db = juce::jmap (y, graticule.getBottom(), graticule.getY(), -eqRangeDb, eqRangeDb);
    return juce::jlimit (-eqRangeDb, eqRangeDb, std::round (db / eqStepDb) * eqStepDb);
}

float CrtDisplay::yForGain (float gainDb) const
{
    return juce::jmap (gainDb, -eqRangeDb, eqRangeDb, graticule.getBottom(), graticule.getY());
}

void CrtDisplay::setBandGain (int band, float gainDb)
{
    if (juce::approximatelyEqual (eqGains[(size_t) band], gainDb))
        return;
    wake();
    eqGains[(size_t) band] = gainDb;
    bandAccessors[(size_t) band]->gainChanged();
    if (onEqBandChanged != nullptr)
        onEqBandChanged (band, gainDb);
}

void CrtDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (isEqEditing)
    {
        const auto p = toPhosphor (e.position);
        draggedBand = bandAt (p.x);
        keyboardBand = draggedBand;
        setBandGain (draggedBand, gainAt (p.y));
        return;
    }

    if (mode != Mode::gallery || ! isPowered)
        return;
    gallery.rescan();   // picks up pictures added since
    if (gallery.size() == 0)
        return;
    const auto next = (galleryMotion.index + 1) % gallery.size();
    setGalleryIndex (next);
    if (onGalleryIndexChanged != nullptr)
        onGalleryIndexChanged (next);
}

void CrtDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (draggedBand >= 0)
        setBandGain (draggedBand, gainAt (toPhosphor (e.position).y));
}

void CrtDisplay::mouseUp (const juce::MouseEvent&)
{
    if (draggedBand < 0)
        return;
    draggedBand = -1;
    if (pendingGains.has_value())
        setEqGains (*std::exchange (pendingGains, std::nullopt));
    wake();
}

void CrtDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isEqEditing)
        setBandGain (bandAt (toPhosphor (e.position).x), 0.0f);
}

bool CrtDisplay::keyPressed (const juce::KeyPress& key)
{
    if (! isEqEditing)
        return false;

    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        keyboardBand = juce::jlimit (0, columns - 1, keyboardBand + (code == juce::KeyPress::leftKey ? -1 : 1));
        wake();
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const auto step = (key.getModifiers().isShiftDown() ? eqFineStepDb : eqStepDb) * (code == juce::KeyPress::upKey ? 1.0f : -1.0f);
        const auto gain = std::round ((eqGains[(size_t) keyboardBand] + step) / eqFineStepDb) * eqFineStepDb;
        setBandGain (keyboardBand, juce::jlimit (-eqRangeDb, eqRangeDb, gain));
        return true;
    }
    return false;
}

void CrtDisplay::focusGained (FocusChangeType)
{
    wake();
}

void CrtDisplay::focusLost (FocusChangeType)
{
    wake();
}
