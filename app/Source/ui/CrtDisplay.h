#pragma once

#include "audio/AudioEngine.h"
#include "ui/crt/CpuCrtRenderer.h"
#include "ui/crt/CrtArtwork.h"
#include "ui/crt/EqBandAccessor.h"
#include "ui/crt/CrtTraces.h"
#include "ui/crt/Gallery.h"
#include <array>
#include <optional>

#include "ui/crt/GpuCrtView.h"

// The CRT: a phosphor visualiser (spectrum / triggered waveform / X-Y), a phosphor picture gallery
// or, in EQ mode, ten draggable phosphor points for the graphic equaliser. Builds a Crt::Scene
// each display frame and hands it to the GPU renderer (Metal on macOS; the CPU renderer where there is none or it fails).
// At power-on the OscillaFX butterfly burns onto the glass during the warm-up, then fades; a silent
// scope keeps it very dimly lit. Stops rendering once the picture is static, and while the window is hidden or occluded. The GPU
// renderer is created on first show (never for a window that stays hidden in the menu bar).
// In EQ mode the display takes keyboard focus (left/right band, up/down 0.5 dB, shift 0.1 dB) and
// exposes each band to accessibility clients as a slider ("EQ 62 Hz").
class CrtDisplay : public juce::Component
{
public:
    enum class Mode { spectrum = 0, wave = 1, xy = 2, gallery = 3 };
    using EqGains = std::array<float, DspSettings::numEqBands>;

    explicit CrtDisplay (AudioEngine&);
    ~CrtDisplay() override;

    void setPowered (bool);
    void setMode (Mode);
    void setEqEditing (bool);
    void setEqGains (const EqGains&);
    void setMessage (const juce::String&);
    void setEqFrequencies (const std::array<float, DspSettings::numEqBands>&);  // band labels / titles
    void setGalleryIndex (int);
    int galleryCount() const;

    // One line of plain-language help in crisp teal phosphor type at the bottom of the screen (top
    // while the EQ is edited). Fades in and out; an empty string clears it.
    void setHoverHint (const juce::String&);

    // Auto-advance through the pictures every `seconds` (2..120), with the burn cross-fade; each step
    // fires onGalleryIndexChanged. Only runs while the gallery is showing.
    void setGallerySlideshow (bool enabled, int seconds);

    // Re-lists the gallery folders right now (after the user imported pictures). The index is kept
    // when it is still valid, else clamped.
    void reloadGallery();

    std::function<void (int band, float gainDb)> onEqBandChanged;
    std::function<void (int newIndex)> onGalleryIndexChanged;   // a click on the screen in gallery mode

    // Dev/snapshot support: the tube face as last rendered (device pixels; GPU read-back on Metal),
    // and keep animating while the window is occluded (headless snapshot runs, display asleep).
    juce::Image renderedScreen();
    void setRendersWhenOccluded (bool);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;

private:
    static constexpr int scopeCapacity = 4096;
    static constexpr int triggerProbePoints = 64;

    struct GalleryMotion
    {
        int index = 0;
        float burn = 0.0f;              // 1 right after switching picture, decays to 0
        double nextTear = 4.0, tearStart = -10.0;
        float tearDuration = 0.35f, tearY = 0.5f, tearHeight = 0.08f, tearSeed = 0.0f;
    };

    void tick (double timestampSeconds);
    void ensureRenderer();
    void fallBackToCpu();
    void wake();
    void updateCursor();
    bool isStatic() const;
    bool isOnScreen() const;
    void advancePower (float dt);
    void readSignal (float dt);

    void buildScene (float dt);
    float freshDensity (float energy) const;
    void addSpectrum();
    void addWave();
    void addXY();
    void addEq();
    void updateGallery (float dt);
    void updateSplash();
    void updateOverlay();
    void updateHint (float dt);
    bool isHintSettled() const;
    void advanceSlideshow();
    std::vector<Crt::Artwork::Label> currentLabels() const;
    juce::Point<int> overlayPixels() const;
    float persistence() const;
    int findTrigger (int window);

    juce::Point<float> toPhosphor (juce::Point<float> screenPosition) const;
    juce::String bandLabel (int band) const;
    int highlightedBand() const;
    void layOutBandAccessors();
    int bandAt (float x) const;
    float gainAt (float y) const;
    float yForGain (float gainDb) const;
    void setBandGain (int band, float gainDb);

    AudioEngine& engine;
    std::unique_ptr<GpuCrtView> gpu;
    bool hasTriedGpu = false;
    CpuCrtRenderer cpu;
    juce::VBlankAttachment vblank;
    Crt::Scene scene;
    juce::Rectangle<float> graticule;

    Mode mode = Mode::spectrum;
    bool isPowered = true;
    bool isEqEditing = false;
    float beam = 0.0f;              // 0 dark .. 1 full; warms up / collapses on power changes
    float poweredSeconds = 0.0f;    // since the last power-on (degauss)
    float afterglow = 0.0f;
    EqGains eqGains {};
    std::optional<EqGains> pendingGains;   // arrived during a drag, applied on release
    std::array<float, DspSettings::numEqBands> eqFrequencies = DspSettings {}.eqFreqHz;
    std::array<std::unique_ptr<EqBandAccessor>, DspSettings::numEqBands> bandAccessors;
    int draggedBand = -1;
    int keyboardBand = 0;
    juce::String message;

    std::array<float, DspSettings::numEqBands> bands {}, peaks {};
    float bassFast = 0.0f, bassSlow = 0.0f;
    std::vector<float> left, right, mono, trigger, waveShown;
    int sampleCount = 0;
    std::array<float, triggerProbePoints> lastSweep {};
    bool isSilent = true;

    Crt::LuminanceImage splashImage;   // the butterfly mark, burned in at power-on and left dimly lit when idle
    Crt::Gallery gallery;
    GalleryMotion galleryMotion;
    int shownPictureId = 0;

    juce::Image overlayImage, hintImage;
    std::vector<Crt::Artwork::Label> overlayLabels;
    juce::Point<int> overlaySize, hintSize;

    juce::String hintWanted, hintShown, hintRenderedText;   // wanted: what the panel asked for; shown: what is on the glass now
    bool hintOnTop = false;
    float hintLevel = 0.0f;

    bool slideshowOn = false;
    double slideshowSeconds = 8.0, slideshowNext = 0.0;

    Crt::Traces::LevelFollower waveGain { 0.7f }, xyGain { 0.85f };
    juce::Point<int> renderedPixels;   // GPU drawable size the last frame was built for

    double lastFrame = 0.0, clock = 0.0;
    float quietSeconds = 0.0f;      // how long the picture has been static
    bool reducedMotion = false;
    bool rendersWhenOccluded = false;
    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrtDisplay)
};
