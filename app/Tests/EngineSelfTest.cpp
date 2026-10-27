// Headless checks for the audio platform layer. Never starts routing or touches the system default device.
#include "audio/AudioEngine.h"
#include "audio/DriftResampler.h"
#include "audio/Dynamics.h"
#include "audio/DspParams.h"
#include "audio/SpscRingBuffer.h"
#include "audio/TripleBuffer.h"
#include "core/AppPaths.h"
#include "core/AppPrefs.h"
#include "core/Controller.h"
#include "core/DeviceKind.h"
#include "core/GalleryStore.h"
#include "core/Presets.h"
#include "core/StarterProfiles.h"
#include "core/SuggestionTracker.h"
#include "ImageFixtures.h"
#include "platform/CrashWatchdog.h"
#include "profiles/ProfileStore.h"
#include "DfxDsp.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <random>
#include <thread>
#include <tuple>

#if JUCE_MAC
 #include "audio/mac/CoreAudioHelpers.h"
 #include <unistd.h>
#endif
#if JUCE_WINDOWS
 #include "audio/win/DefaultRoles.h"
 #include "audio/win/EndpointTraits.h"
 #include "audio/win/WasapiFormat.h"
 #include "platform/LoginItem.h"
 #include "platform/win/ImageHeader.h"
 #include "platform/win/WicImage.h"
 #include <juce_graphics/juce_graphics.h>
 #include <stdlib.h>
#endif

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

bool near (double a, double b, double tolerance) { return std::abs (a - b) <= tolerance; }

void setEnvironment (const char* name, const juce::String& value)
{
   #if JUCE_WINDOWS
    _putenv_s (name, value.toRawUTF8());   // an empty value removes the variable
   #else
    if (value.isEmpty())
        ::unsetenv (name);
    else
        ::setenv (name, value.toRawUTF8(), 1);
   #endif
}

void testDeviceListing()
{
    std::printf ("devices\n");
   #if JUCE_MAC
    for (auto id : CoreAudioHelpers::allDevices())
        std::printf ("  %-32s uid=%-40s out=%d builtin=%d rate=%.0f\n",
                     CoreAudioHelpers::deviceName (id).toRawUTF8(), CoreAudioHelpers::deviceUid (id).toRawUTF8(),
                     CoreAudioHelpers::outputChannelCount (id), (int) CoreAudioHelpers::isBuiltIn (id),
                     CoreAudioHelpers::nominalSampleRate (id));
   #endif

    AudioEngine engine; // constructed only; start() is never called here
    auto outputs = engine.outputDevices();
    auto current = engine.currentOutputDevice();
    std::printf ("  engine outputs=%d current=\"%s\"\n", outputs.size(), current.name.toRawUTF8());
    bool listsVirtual = false;
    for (auto& d : outputs)
        listsVirtual |= d.uid == "Oscilla_UID";
    check (! listsVirtual, "virtual device excluded from outputs");
    check (outputs.isEmpty() || current.uid.isNotEmpty(), "current output resolved");

    float bands[10];
    float left[64], right[64];
    engine.getSpectrum (bands);
    check (engine.getScopeSamples (left, right, 64) == 0, "scope empty before start");
}

DspSettings sampleSettings()
{
    DspSettings s;
    s.power = false;
    s.presetName = "Custom";
    s.fidelity = 7.5f;
    s.bass = 9.0f;
    s.eqGainDb[3] = -4.0f;
    s.eqFreqHz[2] = 250.0f;
    s.masterGainDb = -6.0f;
    s.balance = -0.25f;
    s.displayMode = 2;
    return s;
}

void testProfileStore()
{
    std::printf ("profile store\n");
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("oscilla_selftest_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    {
        ProfileStore store (dir);
        check (store.load ("anything") == DspSettings {}, "empty store returns defaults");
        auto s = sampleSettings();
        store.save ("AppleUSBAudioEngine:Vendor:Thing:1/2", s);
        check (store.load ("AppleUSBAudioEngine:Vendor:Thing:1/2") == s, "round trip");
        check (store.load ("never-seen") == s, "unknown device copies last used");
    }
    {
        ProfileStore reopened (dir);
        check (reopened.load ("never-seen") == sampleSettings(), "last used survives restart");
        auto other = DspSettings {};
        other.bass = 1.0f;
        reopened.save ("BuiltInSpeakerDevice", other);
        check (reopened.load ("BuiltInSpeakerDevice") == other
               && reopened.load ("AppleUSBAudioEngine:Vendor:Thing:1/2") == sampleSettings(), "profiles independent");
    }
    auto clamped = DspSettings::fromVar (juce::JSON::parse (R"({"bass": 99, "eqGainDb": [40, -40], "balance": 3})"));
    check (near (clamped.bass, 10.0, 0) && near (clamped.eqGainDb[0], 12.0, 0) && near (clamped.eqGainDb[1], -12.0, 0)
           && near (clamped.balance, 1.0, 0) && near (clamped.fidelity, DspSettings {}.fidelity, 0), "fromVar clamps and defaults");
    {
        ProfileStore store (dir);
        store.save ("corrupt-device", sampleSettings());
        auto files = dir.findChildFiles (juce::File::findFiles, false, "corrupt-device*.json");
        check (files.size() == 1, "profile file created");
        if (files.size() == 1)
        {
            files[0].replaceWithText ("{ not json");
            check (store.load ("corrupt-device") == sampleSettings() || store.load ("corrupt-device") == DspSettings {},
                   "corrupt profile falls back instead of failing");
            store.save ("corrupt-device", DspSettings {});
            auto backup = files[0].getSiblingFile (files[0].getFileName() + ".bak");
            check (backup.loadFileAsString() == "{ not json", "corrupt profile kept as .bak");
            check (store.load ("corrupt-device") == DspSettings {}, "rewritten profile readable");
        }
    }
    dir.deleteRecursively();
}

void testRingBufferThreaded()
{
    std::printf ("spsc ring\n");
    SpscRingBuffer ring (1024);
    static constexpr int TOTAL = 2'000'000;   // static: MSVC will not use an uncaptured local in the lambdas
    std::thread producer ([&ring]
    {
        std::mt19937 rng (1);
        float block[2 * 300];
        for (int sent = 0; sent < TOTAL;)
        {
            int n = std::min ((int) (rng() % 300) + 1, TOTAL - sent);
            for (int i = 0; i < n; ++i)
                block[2 * i] = block[2 * i + 1] = (float) ((sent + i) % 1000000);
            int done = (int) ring.write (block, (size_t) n);
            sent += done;
            if (done < n)
                std::this_thread::yield();
        }
    });
    bool ordered = true;
    for (int received = 0; received < TOTAL;)
    {
        auto available = ring.availableToRead();
        for (size_t i = 0; i < available; ++i, ++received)
        {
            const float* f = ring.peek (i);
            ordered &= near (f[0], received % 1000000, 0) && near (f[1], f[0], 0);
        }
        ring.consume (available);
    }
    producer.join();
    check (ordered, "2M frames arrive in order across threads");
}

struct DriftResult
{
    int underrunsAfterPriming;
    double finalRatio, fillError, maxStep;
};

// Event-driven simulation of two free-running device clocks feeding through ring + resampler.
DriftResult simulateDrift (double inputRate, double outputRate, double seconds)
{
    constexpr int IN_BLOCK = 512, OUT_BLOCK = 470;
    constexpr double TONE_HZ = 440.0, AMPLITUDE = 0.5;
    SpscRingBuffer ring (1 << 16);
    DriftResampler resampler;
    resampler.prepare (inputRate, outputRate, 2 * (IN_BLOCK + OUT_BLOCK));

    std::vector<float> in ((size_t) IN_BLOCK * 2), out ((size_t) OUT_BLOCK * 2);
    double tIn = 0, tOut = 0;
    long long inFrames = 0;
    int primedUnderruns = 0;
    bool everPrimed = false;
    double maxStep = 0, fillErrorSum = 0;
    int fillSamples = 0;
    float lastSample = 0;

    while (tOut < seconds)
    {
        if (tIn <= tOut)
        {
            for (int i = 0; i < IN_BLOCK; ++i, ++inFrames)
                in[(size_t) (2 * i)] = in[(size_t) (2 * i + 1)] = (float) (AMPLITUDE * std::sin (2 * M_PI * TONE_HZ * (double) inFrames / inputRate));
            ring.write (in.data(), IN_BLOCK);
            tIn += IN_BLOCK / inputRate;
            continue;
        }
        const bool rendered = resampler.render (ring, out.data(), OUT_BLOCK);
        tOut += OUT_BLOCK / outputRate;
        if (! rendered)
        {
            primedUnderruns += everPrimed ? 1 : 0;
            continue;
        }
        if (everPrimed)
            maxStep = std::max (maxStep, (double) std::abs (out[0] - lastSample));
        everPrimed = true;
        for (int i = 1; i < OUT_BLOCK; ++i)
            maxStep = std::max (maxStep, (double) std::abs (out[(size_t) (2 * i)] - out[(size_t) (2 * (i - 1))]));
        lastSample = out[(size_t) (2 * (OUT_BLOCK - 1))];
        if (tOut > seconds * 0.75)
        {
            fillErrorSum += std::abs (resampler.smoothedFill() - (double) resampler.targetFill()) / (double) resampler.targetFill();
            ++fillSamples;
        }
    }
    return { primedUnderruns, resampler.ratio(), fillSamples ? fillErrorSum / fillSamples : 1.0, maxStep };
}

void testDriftCompensation()
{
    std::printf ("drift resampler\n");
    const double maxSineStep = 2 * M_PI * 440.0 / 48000.0 * 0.5 * 1.05;
    struct Case { double inRate, outRate; const char* name; };
    for (auto c : { Case { 48000.0 * (1 + 300e-6), 48000.0, "+300 ppm" },
                    Case { 48000.0 * (1 - 300e-6), 48000.0, "-300 ppm" },
                    Case { 44100.0, 48000.0, "44.1k -> 48k nominal mismatch" } })
    {
        auto r = simulateDrift (c.inRate, c.outRate, 180.0);
        const double expected = c.inRate / c.outRate;
        std::printf ("  %s: ratio=%.6f (expect %.6f) fillErr=%.3f underruns=%d maxStep=%.4f\n",
                     c.name, r.finalRatio, expected, r.fillError, r.underrunsAfterPriming, r.maxStep);
        check (r.underrunsAfterPriming == 0, juce::String (c.name) + ": no underruns after priming");
        check (near (r.finalRatio / expected, 1.0, 50e-6), juce::String (c.name) + ": ratio locks to clock ratio");
        check (r.fillError < 0.25, juce::String (c.name) + ": fill settles near target");
        check (r.maxStep < maxSineStep * c.inRate / c.outRate * 1.1, juce::String (c.name) + ": output continuous");
    }
}

void testTripleBuffer()
{
    std::printf ("triple buffer\n");
    TripleBuffer<DspParams> box;
    DspParams out;
    check (! box.fetch (out), "nothing before publish");
    DspParams a, b;
    a.masterGainDb = 1.0f;
    b.masterGainDb = 2.0f;
    box.publish (a);
    box.publish (b);
    check (box.fetch (out) && near (out.masterGainDb, 2.0, 0), "reader sees latest");
    check (! box.fetch (out), "no repeat without new publish");
}

void testDspMapping()
{
    std::printf ("dsp mapping\n");
    DfxDsp dsp;
    dsp.setSignalFormat (32, 2, 48000, 32);
    DspParamsApply::configureBands (dsp);
    check (dsp.getNumEqBands() == DspSettings::numEqBands, "10 EQ bands");
    auto s = sampleSettings();
    DspParamsApply::applyAll (dsp, DspParams::fromSettings (s));
    DspSettings back = s;
    back.fidelity = back.bass = -1.0f;
    back.eqGainDb.fill (99.0f);
    DspParamsApply::readInto (dsp, back);
    check (near (back.fidelity, 7.5, 1e-4) && near (back.bass, 9.0, 1e-4), "effects 0..10 round trip");
    check (near (back.eqGainDb[3], -4.0, 1e-4) && near (back.eqFreqHz[2], 250.0, 1.0), "eq gain/freq round trip");
    check (near (dsp.getBalance(), -5.0, 1e-4) && near (dsp.getMasterGain(), -6.0, 1e-4), "balance -> dB, master gain");
    check (! dsp.isPowerOn(), "power off maps to bypass");

    auto next = DspParams::fromSettings (s);
    next.effects[4] = 2.0f;
    DspParamsApply::applyChanges (dsp, next, DspParams::fromSettings (s));
    check (near (dsp.getEffectValue (DfxDsp::Bass) * 10.0f, 2.0, 1e-4), "incremental change applied");
}

std::vector<float> testTone()
{
    std::vector<float> tone ((size_t) 48000 * 2 * 8);
    for (size_t i = 0; i < tone.size() / 2; ++i)
        tone[2 * i] = tone[2 * i + 1] = (float) (0.3 * std::sin (2 * M_PI * 120.0 * (double) i / 48000.0)
                                                + 0.1 * std::sin (2 * M_PI * 5000.0 * (double) i / 48000.0));
    return tone;
}

// Mirrors the engine: full apply on the processing thread, then frequent incremental changes.
std::vector<float> runLiveSequence (DfxDsp& dsp, const DspParams& first, const DspParams& second)
{
    constexpr int BLOCK = 480, CHANGE_EVERY_BLOCKS = 10;
    auto in = testTone();
    std::vector<float> out (in.size());
    DspParamsApply::applyAll (dsp, first);
    const int frames = (int) in.size() / 2;
    for (int i = 0; i + BLOCK <= frames; i += BLOCK)
    {
        const int block = i / BLOCK;
        if (block % CHANGE_EVERY_BLOCKS == 0 && block > 0)
        {
            const bool toSecond = (block / CHANGE_EVERY_BLOCKS) % 2 == 1;
            DspParamsApply::applyChanges (dsp, toSecond ? second : first, toSecond ? first : second);
        }
        dsp.processAudio ((short*) &in[(size_t) (2 * i)], (short*) &out[(size_t) (2 * i)], BLOCK, 0);
    }
    return out;
}

void prepareLiveDsp (DfxDsp& dsp)
{
    dsp.setSignalFormat (32, 2, 48000, 32);
    DspParamsApply::configureBands (dsp);
}

void testRegistryIsolation()
{
    std::printf ("thread_local dsp registry\n");
    auto first = DspParams::fromSettings (sampleSettings());
    first.power = true;
    auto second = first;
    second.eqGainDb[1] = 6.0f;
    second.effects[4] = 1.0f;

    DfxDsp reference;
    prepareLiveDsp (reference);
    auto expected = runLiveSequence (reference, first, second);

    // Live instance: constructed on this thread, driven from another, while this thread
    // hammers the registry through preset loads (the old shared table let these leak across).
    DfxDsp live;
    prepareLiveDsp (live);
    DspParamsApply::applyAll (live, DspParams::fromSettings (DspSettings {}));
    std::vector<float> actual;
    std::atomic<bool> done { false };
    std::thread capture ([&] { actual = runLiveSequence (live, first, second); done = true; });
    int presetLoads = 0;
    while (! done)
    {
        Presets::load ("Metal", DspSettings {});
        ++presetLoads;
    }
    capture.join();
    std::printf ("  %d concurrent preset loads\n", presetLoads);
    check (actual == expected, "worker-thread output bit-exact vs single-thread reference");
}

void testPresets()
{
    std::printf ("presets\n");
    for (const auto& dir : Presets::directories())
        std::printf ("  dir %s\n", dir.getFullPathName().toRawUTF8());
    auto names = Presets::names();
    std::printf ("  %d presets: %s ...\n", names.size(), names.joinIntoString (", ").substring (0, 80).toRawUTF8());
    check (names.contains ("Pop"), "Pop preset listed");
    check (names.size() >= 2 && names[0] == "Movies" && names[1] == "Music", "Movies, Music listed first");
    auto unique = names;
    unique.removeDuplicates (false);
    check (unique.size() == names.size(), "no duplicate names");
    check (Presets::load ("Movies", DspSettings {}).has_value() && Presets::load ("Music", DspSettings {}).has_value(),
           "Movies and Music load");
    DspSettings base;
    base.masterGainDb = -3.0f;
    auto a = Presets::load ("Pop", base), b = Presets::load ("Pop", base);
    check (a.has_value() && b.has_value() && *a == *b, "deterministic load");
    if (a)
    {
        std::printf ("  Pop: fid=%.2f amb=%.2f sur=%.2f dyn=%.2f bass=%.2f eq0=%.1f@%.0fHz\n",
                     a->fidelity, a->ambience, a->surround, a->dynamicBoost, a->bass, a->eqGainDb[0], a->eqFreqHz[0]);
        check (a->presetName == "Pop" && near (a->masterGainDb, -3.0, 0) && a->bass > 0.0f, "preset values mapped, gain kept");
    }
    check (! Presets::load ("does-not-exist", base).has_value(), "missing preset rejected");
}
// The record the crash watchdog trusts. Backs up and restores any real record around the test.
void testRestoreRecord()
{
    std::printf ("restore record\n");
    auto file = CrashWatchdog::restoreFile();
    const bool hadRecord = file.existsAsFile();
    const auto saved = file.loadFileAsString();

    check (CrashWatchdog::writeRestoreUid ("SelfTest_UID"), "record written");
    check (file.loadFileAsString() == juce::String (CrashWatchdog::currentProcessId()) + "\nSelfTest_UID\n", "record is <pid>\\n<uid>");
    check (CrashWatchdog::readRestoreUid() == "SelfTest_UID", "uid read back");
    check (CrashWatchdog::readRestoreRoles() == CrashWatchdog::DEFAULT_ROLES, "2-line record: console | multimedia roles");
    const auto pid = juce::String (CrashWatchdog::currentProcessId());
    check (CrashWatchdog::writeRestoreUid ("Roles_UID", CrashWatchdog::consoleRole | CrashWatchdog::communicationsRole)
           && file.loadFileAsString() == pid + "\nRoles_UID\n5\n", "record with roles is <pid>\\n<uid>\\n<roles>");
    check (CrashWatchdog::readRestoreUid() == "Roles_UID" && CrashWatchdog::readRestoreRoles() == 5, "uid and roles read back");
    CrashWatchdog::writeRestoreUid ("Masked_UID", 0xff);
    check (CrashWatchdog::readRestoreRoles() == CrashWatchdog::ALL_ROLES, "roles masked to the three roles");
    file.replaceWithText (pid + "\nZero_UID\n0\n");
    check (CrashWatchdog::readRestoreRoles() == CrashWatchdog::DEFAULT_ROLES, "roles 0 -> console | multimedia");
    file.replaceWithText ("Legacy_UID");
    check (CrashWatchdog::readRestoreUid() == "Legacy_UID", "legacy single-line record read");
    CrashWatchdog::clearRestoreUid();
    check (! file.exists() && CrashWatchdog::readRestoreUid().isEmpty(), "record cleared");

    if (hadRecord)
        file.replaceWithText (saved);
}

// ---------------------------------------------------------------------------------- backend

juce::File makeTempDir (const char* tag)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile (juce::String ("oscilla_selftest_") + tag + "_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    dir.createDirectory();
    return dir;
}

// ---- device kind classifier

void testDeviceKinds()
{
    std::printf ("device kind classifier\n");
    constexpr auto cc = [] (const char (&c)[5])
    {
        return ((juce::uint32) (unsigned char) c[0] << 24) | ((juce::uint32) (unsigned char) c[1] << 16)
             | ((juce::uint32) (unsigned char) c[2] << 8) | (juce::uint32) (unsigned char) c[3];
    };
    const auto builtin = cc ("bltn"), usb = cc ("usb "), blue = cc ("blue"), ble = cc ("blea"), hdmi = cc ("hdmi"),
               dprt = cc ("dprt"), airp = cc ("airp"), thun = cc ("thun"), virt = cc ("virt"), grup = cc ("grup");
    const auto ispk = cc ("ispk"), hdpn = cc ("hdpn"), espk = cc ("espk");

    struct Case { const char* label; const char* uid; const char* name; juce::uint32 transport, source; DeviceKind expected; };
    const Case cases[] = {
        { "MacBook speakers",          "BuiltInSpeakerDevice",           "MacBook Pro Speakers",       builtin, ispk, DeviceKind::builtInSpeakers },
        { "iMac speakers",             "BuiltInSpeakerDevice",           "iMac Speakers",              builtin, ispk, DeviceKind::builtInSpeakers },
        { "Mac mini speaker, no src",  "BuiltInSpeakerDevice",           "Mac mini Speakers",          builtin, 0,    DeviceKind::builtInSpeakers },
        { "jack by data source",       "BuiltInSpeakerDevice",           "MacBook Pro Speakers",       builtin, hdpn, DeviceKind::headphonesWired },
        { "jack by uid",               "BuiltInHeadphoneOutputDevice",   "External Headphones",        builtin, 0,    DeviceKind::headphonesWired },
        { "jack by name",              "x",                              "Headphones",                 builtin, 0,    DeviceKind::headphonesWired },
        { "line out",                  "BuiltInLineOutputDevice",        "Line Out",                   builtin, 0,    DeviceKind::externalSpeakers },
        { "external speaker source",   "BuiltInSpeakerDevice",           "External Speakers",          builtin, espk, DeviceKind::externalSpeakers },
        { "AirPods Pro",               "AA-BB-CC",                       "Efti's AirPods Pro",         blue,    0,    DeviceKind::airPods },
        { "AirPods Max LE",            "AA",                             "AirPods Max",                ble,     0,    DeviceKind::airPods },
        { "Beats",                     "AA",                             "Beats Studio Pro",           blue,    0,    DeviceKind::airPods },
        { "Sony BT headphones",        "AA",                             "WH-1000XM5 Headphones",      blue,    0,    DeviceKind::headphonesBluetooth },
        { "BT unnamed",                "AA",                             "WH-1000XM5",                 blue,    0,    DeviceKind::headphonesBluetooth },
        { "BT speaker",                "AA",                             "JBL Flip 6 Speaker",         blue,    0,    DeviceKind::externalSpeakers },
        { "BT headset w/ speaker word","AA",                             "Speaker Headset Pro",        blue,    0,    DeviceKind::headphonesBluetooth },
        { "HDMI",                      "AppleGFXHDAEngineOutputDP",      "LG HDR 4K",                  hdmi,    0,    DeviceKind::display },
        { "DisplayPort",               "x",                              "DELL U2720Q",                dprt,    0,    DeviceKind::display },
        { "USB studio display",        "x",                              "Studio Display Speakers",    usb,     0,    DeviceKind::display },
        { "USB headset",               "x",                              "Logitech G Pro Headset",     usb,     0,    DeviceKind::headphonesUsb },
        { "USB DAC",                   "x",                              "USB Audio DAC",              usb,     0,    DeviceKind::headphonesUsb },
        { "USB monitors",              "x",                              "KRK Monitor Interface",      usb,     0,    DeviceKind::externalSpeakers },
        { "Thunderbolt interface",     "x",                              "Apollo Twin",                thun,    0,    DeviceKind::headphonesUsb },
        { "AirPlay",                   "x",                              "Living Room",                airp,    0,    DeviceKind::externalSpeakers },
        { "aggregate",                 "x",                              "My Aggregate",               grup,    0,    DeviceKind::unknown },
        { "virtual",                   "x",                              "BlackHole 16ch",             virt,    0,    DeviceKind::unknown },
        { "no transport, AirPods name","x",                              "AirPods Pro",                0,       0,    DeviceKind::airPods },
        { "no transport, MacBook",     "x",                              "MacBook Pro Speakers",       0,       0,    DeviceKind::builtInSpeakers },
        { "no transport, TV",          "x",                              "Samsung TV",                 0,       0,    DeviceKind::display },
        { "word-match: 'tv' inside",   "x",                              "Activate Output",            0,       0,    DeviceKind::unknown },
        { "no transport, nothing",     "",                               "",                           0,       0,    DeviceKind::unknown },
    };
    for (const auto& c : cases)
    {
        const auto got = DeviceKinds::classify ({ c.uid, c.name, c.transport, c.source });
        check (got == c.expected, juce::String (c.label) + " -> " + DeviceKinds::id (c.expected)
                                      + (got == c.expected ? "" : " (got " + DeviceKinds::id (got) + ")"));
    }

    juce::StringArray ids;
    for (auto kind : { DeviceKind::builtInSpeakers, DeviceKind::headphonesWired, DeviceKind::headphonesUsb,
                       DeviceKind::headphonesBluetooth, DeviceKind::airPods, DeviceKind::display,
                       DeviceKind::externalSpeakers, DeviceKind::unknown })
    {
        ids.add (DeviceKinds::id (kind));
        check (DeviceKinds::label (kind).isNotEmpty(), "label for " + DeviceKinds::id (kind));
    }
    ids.removeDuplicates (false);
    check (ids.size() == 8, "kind ids are unique");

   #if JUCE_MAC
    // Live facts: whatever CoreAudio reports for this machine's outputs must classify without surprises.
    for (auto id : CoreAudioHelpers::allDevices())
        if (CoreAudioHelpers::outputChannelCount (id) > 0)
        {
            const auto traits = DeviceKinds::traitsFor (CoreAudioHelpers::deviceUid (id), CoreAudioHelpers::deviceName (id));
            std::printf ("  live: %-30s transport=%08x source=%08x -> %s\n", traits.name.toRawUTF8(), traits.transport,
                         traits.dataSource, DeviceKinds::id (DeviceKinds::classify (traits)).toRawUTF8());
        }
   #endif
}

// ---- starter profiles

void testStarterProfiles()
{
    std::printf ("starter profiles\n");
    for (auto kind : { DeviceKind::builtInSpeakers, DeviceKind::headphonesWired, DeviceKind::headphonesUsb,
                       DeviceKind::headphonesBluetooth, DeviceKind::airPods, DeviceKind::display,
                       DeviceKind::externalSpeakers, DeviceKind::unknown })
    {
        const auto options = StarterProfiles::optionsFor (kind);
        const auto tag = DeviceKinds::id (kind);
        bool sane = options.size() == 5;
        juce::StringArray ids, names;
        for (auto& o : options)
        {
            ids.add (o.id);
            names.add (o.name);
            sane &= o.description.isNotEmpty() && o.settings.presetName == o.name && o.settings.power && o.settings.eqFreqHz == DspSettings {}.eqFreqHz
                 && ! o.settings.nightMode && ! o.settings.limiterEnabled() && near (o.settings.masterGainDb, 0, 0);
            auto s = o.settings;
            sane &= DspSettings::fromVar (s.toVar()) == s; // already inside the clamped ranges
        }
        check (sane, tag + ": five complete starters in range");
        check (names.joinIntoString (",") == "Flat,Warm,Bass-heavy,Voice,Wide"
               && ids.joinIntoString (",") == "flat,warm,bass-heavy,voice,wide", tag + ": Flat, Warm, Bass-heavy, Voice, Wide in order");
    }
    check (StarterProfiles::find (DeviceKind::airPods, "warm").has_value() && ! StarterProfiles::find (DeviceKind::airPods, "nope").has_value(),
           "find by id");

    auto speakers = StarterProfiles::optionsFor (DeviceKind::builtInSpeakers)[0].settings;
    check (speakers.eqGainDb[0] < -3.0f && speakers.eqGainDb[6] > 1.0f, "laptop speakers Flat: low-cut + presence");
    auto headphones = StarterProfiles::optionsFor (DeviceKind::headphonesWired)[0].settings;
    check (headphones.eqGainDb[0] > 0.5f && headphones.eqGainDb[9] > 1.0f, "headphones Flat: bass + air");

    DspSettings base;
    base.power = false;
    base.masterGainDb = -4.0f;
    base.balance = 0.3f;
    base.nightMode = true;
    base.limiterCeilingDb = -9.0f;
    base.displayMode = 2;
    base.galleryIndex = 3;
    auto warm = *StarterProfiles::find (DeviceKind::headphonesWired, "warm");
    auto applied = StarterProfiles::applyTo (base, warm);
    check (applied.presetName == "Warm" && applied.eqGainDb == warm.settings.eqGainDb && near (applied.bass, warm.settings.bass, 0),
           "applyTo takes effects, EQ and name");
    check (! applied.power && near (applied.masterGainDb, -4, 0) && near (applied.balance, 0.3, 1e-6) && applied.nightMode
           && near (applied.limiterCeilingDb, -9, 0) && applied.displayMode == 2 && applied.galleryIndex == 3,
           "applyTo keeps power, gain, balance, night mode, limiter, display state");
}

// ---- profile store flag + suggestion tracker

DeviceKinds::Traits speakerTraits (const char* uid = "BuiltInSpeakerDevice")
{
    return { uid, "MacBook Pro Speakers", 0x626c746e /* bltn */, 0x6973706b /* ispk */ };
}

void testSuggestionFlag()
{
    std::printf ("suggestion flag in profile store\n");
    auto dir = makeTempDir ("flag");
    {
        ProfileStore store (dir);
        check (! store.suggestionHandled ("new"), "unknown device: not handled");
        store.save ("new", DspSettings {});
        check (! store.suggestionHandled ("new"), "profile created by this app: still not handled");
        store.markSuggestionHandled ("new", sampleSettings());
        check (store.suggestionHandled ("new") && store.load ("new") == sampleSettings(), "marked handled, profile written");
        store.save ("new", DspSettings {});
        check (store.suggestionHandled ("new"), "later saves keep the flag");
    }
    {
        ProfileStore reopened (dir);
        check (reopened.suggestionHandled ("new"), "flag survives restart");
        check (reopened.load ("new") == DspSettings {}, "settings from the last save");
    }
    {
        // A legacy profile has no flag: the device is established, never nagged.
        ProfileStore store (dir);
        store.save ("legacy", DspSettings {});
        auto files = dir.findChildFiles (juce::File::findFiles, false, "legacy_*.json");
        auto parsed = juce::JSON::parse (files[0]);
        parsed.getDynamicObject()->removeProperty ("suggestionHandled");
        files[0].replaceWithText (juce::JSON::toString (parsed));
        check (store.suggestionHandled ("legacy"), "profile without the key counts as handled");
        store.save ("legacy", sampleSettings());
        check (store.suggestionHandled ("legacy") && ! juce::JSON::parse (files[0]).hasProperty ("suggestionHandled"),
               "saving a legacy profile does not add the key");
        files[0].replaceWithText ("{ broken");
        check (! store.suggestionHandled ("legacy"), "corrupt profile: not handled");
    }
    dir.deleteRecursively();
}

int countProfiles (const juce::File& dir) { return dir.findChildFiles (juce::File::findFiles, false, "*.json").size(); }

void testSuggestionTracker()
{
    std::printf ("suggestion tracker\n");
    auto dir = makeTempDir ("tracker");
    {
        ProfileStore store (dir);
        store.markSuggestionHandled ("last-used-device", sampleSettings());
        const auto before = countProfiles (dir);

        SuggestionTracker tracker (store);
        check (tracker.deviceActivated (speakerTraits ("brand-new")), "new device: suggestion appears");
        auto pending = tracker.pending();
        check (pending && pending->deviceUid == "brand-new" && pending->deviceName == "MacBook Pro Speakers"
               && pending->deviceKind == "builtin-speakers" && pending->options.size() == 5 && pending->options[0].id == "flat",
               "suggestion carries device, kind and five options");
        check (store.load ("brand-new") == sampleSettings() && countProfiles (dir) == before,
               "nothing changed: new device still copies last-used, no file written");
        check (! tracker.deviceActivated (speakerTraits ("brand-new")), "same device again: no change reported");
        check (tracker.deviceActivated (speakerTraits ("last-used-device")) == true && ! tracker.pending(),
               "device with a profile: suggestion goes away (reported as a change)");
        check (! tracker.deviceActivated (speakerTraits ("last-used-device")), "still nothing, no change reported");

        // dismiss for the session
        tracker.deviceActivated (speakerTraits ("brand-new"));
        check (tracker.dismiss (false, sampleSettings()) && ! tracker.pending(), "dismiss closes it");
        check (! tracker.deviceActivated (speakerTraits ("brand-new")) && ! tracker.pending(), "not asked again this session");
        store.save ("brand-new", sampleSettings()); // the app saves the profile when leaving the device
        SuggestionTracker restarted (store);
        check (restarted.deviceActivated (speakerTraits ("brand-new")) && restarted.pending(), "asked again after a restart");

        // don't ask again
        check (restarted.dismiss (true, sampleSettings()) && ! restarted.pending(), "dismiss + don't ask");
        SuggestionTracker later (store);
        check (! later.deviceActivated (speakerTraits ("brand-new")) && ! later.pending(), "never asked again for that device");
        check (later.deviceActivated (speakerTraits ("another-new")) && later.pending(), "other devices still get suggestions");

        // apply
        DspSettings mine = sampleSettings();
        mine.nightMode = true;
        mine.limiterCeilingDb = -12.0f;
        check (! later.apply ("does-not-exist", mine).has_value() && later.pending(), "unknown starter id: nothing happens");
        auto applied = later.apply ("voice", mine);
        auto voice = *StarterProfiles::find (DeviceKind::builtInSpeakers, "voice");
        check (applied && applied->presetName == "Voice" && applied->eqGainDb == voice.settings.eqGainDb
               && applied->nightMode && near (applied->limiterCeilingDb, -12, 0) && near (applied->masterGainDb, mine.masterGainDb, 0),
               "apply returns starter on top of current, keeping safety settings");
        check (! later.pending() && ! later.apply ("voice", mine).has_value(), "apply closes the suggestion");
        check (store.load ("another-new") == *applied && store.suggestionHandled ("another-new"), "applied settings saved and handled");
        SuggestionTracker afterApply (store);
        check (! afterApply.deviceActivated (speakerTraits ("another-new")), "no suggestion after apply, even after restart");

        // manual change
        afterApply.deviceActivated (speakerTraits ("third-new"));
        check (afterApply.userTookControl (mine) && ! afterApply.pending() && store.suggestionHandled ("third-new"),
               "manual edit ends the suggestion for good");
        check (! afterApply.userTookControl (mine), "second manual edit: nothing pending");

        // legacy profile on disk (no flag): never suggested
        store.save ("legacy-device", DspSettings {});
        auto file = dir.findChildFiles (juce::File::findFiles, false, "legacy-device_*.json")[0];
        auto parsed = juce::JSON::parse (file);
        parsed.getDynamicObject()->removeProperty ("suggestionHandled");
        file.replaceWithText (juce::JSON::toString (parsed));
        SuggestionTracker legacy (store);
        check (! legacy.deviceActivated (speakerTraits ("legacy-device")), "device from an older profile (profile without flag): no suggestion");
        check (! legacy.deviceActivated ({ "", "", 0, 0 }), "no device uid: no suggestion");
    }
    dir.deleteRecursively();
}

// ---- gallery store

void writeBytes (const juce::File& file, const unsigned char* data, size_t size)
{
    file.deleteFile();
    file.replaceWithData (data, size);
}

bool hasLeftovers (const juce::File& dir) { return ! dir.findChildFiles (juce::File::findFiles, false, ".oscilla-import-*").isEmpty(); }

void testGalleryStore()
{
    std::printf ("gallery store\n");
    auto root = makeTempDir ("gallery");
    auto user = root.getChildFile ("user");
    auto bundled = root.getChildFile ("bundled");
    auto incoming = root.getChildFile ("incoming");
    auto outside = root.getChildFile ("outside");
    for (auto* d : { &bundled, &incoming, &outside })
        d->createDirectory();
    writeBytes (bundled.getChildFile ("01-bundled.png"), ImageFixtures::png, sizeof (ImageFixtures::png));
    writeBytes (bundled.getChildFile ("20-bundled.jpg"), ImageFixtures::jpg, sizeof (ImageFixtures::jpg));

    GalleryStore store (user, bundled);
    check (store.images().size() == 2 && ! user.exists(), "lists bundled pictures; listing creates nothing");

    struct Format { const char* file; const unsigned char* bytes; size_t size; const char* ext; };
    const Format formats[] = {
        { "photo.png", ImageFixtures::png, sizeof (ImageFixtures::png), "png" },
        { "photo.jpeg", ImageFixtures::jpg, sizeof (ImageFixtures::jpg), "jpg" },
        { "anim.gif", ImageFixtures::gif, sizeof (ImageFixtures::gif), "gif" },
        { "scan.tif", ImageFixtures::tiff, sizeof (ImageFixtures::tiff), "tiff" },
       #if JUCE_MAC   // Windows: only with the Store codecs, checked in testWindowsImageCodecs()
        { "web.webp", ImageFixtures::webp, sizeof (ImageFixtures::webp), "webp" },
        { "phone.heic", ImageFixtures::heic, sizeof (ImageFixtures::heic), "heic" },
       #endif
    };
    const int formatCount = (int) std::size (formats);
    juce::Array<juce::File> batch;
    for (const auto& f : formats)
    {
        auto file = incoming.getChildFile (f.file);
        writeBytes (file, (const unsigned char*) f.bytes, f.size);
        batch.add (file);
    }
    juce::String error;
    check (store.importImages (batch, error) == formatCount && error.isEmpty(), "all " + juce::String (formatCount) + " formats import" + (error.isEmpty() ? juce::String() : " (" + error + ")"));
    for (const auto& f : formats)
        check (user.getChildFile (juce::String (f.file).upToLastOccurrenceOf (".", false, false) + "." + f.ext).existsAsFile(),
               juce::String (f.file) + " stored as ." + f.ext);
    check (store.images().size() == 2 + formatCount, "listing merges user and bundled");
    auto names = store.images();
    bool sorted = true;
    for (int i = 1; i < names.size(); ++i)
        sorted &= names[i - 1].getFileName().compareNatural (names[i].getFileName()) <= 0;
    check (sorted, "listing sorted by name");

    // content decides, not the extension
    auto liar = incoming.getChildFile ("really-a-png.jpg");
    writeBytes (liar, ImageFixtures::png, sizeof (ImageFixtures::png));
    check (store.importImages ({ liar }, error) == 1 && user.getChildFile ("really-a-png.png").existsAsFile(), "PNG named .jpg is stored as .png");
    auto fake = incoming.getChildFile ("fake.png");
    fake.replaceWithText ("<html>definitely not an image</html>");
    check (store.importImages ({ fake }, error) == 0 && error.contains ("fake.png") && ! user.getChildFile ("fake.png").exists(),
           "text file named .png is rejected");
    auto truncated = incoming.getChildFile ("cut.jpg");
    writeBytes (truncated, ImageFixtures::jpg, 40);
    check (store.importImages ({ truncated }, error) == 0 && error.isNotEmpty(), "truncated JPEG is rejected");
    auto empty = incoming.getChildFile ("empty.png");
    empty.replaceWithText ("");
    check (store.importImages ({ empty }, error) == 0 && error.contains ("empty"), "empty file is rejected");
    auto directory = incoming.getChildFile ("folder.png");
    directory.createDirectory();
    check (store.importImages ({ directory, incoming.getChildFile ("missing.png") }, error) == 0 && error.contains ("folder.png") && error.contains ("missing.png"),
           "directory and missing file are rejected, one line each");
    check (! hasLeftovers (user), "no temp files left behind after rejections");

    // size limit
    auto huge = incoming.getChildFile ("huge.png");
    {
        juce::FileOutputStream out (huge);
        out.write (ImageFixtures::png, sizeof (ImageFixtures::png));
        std::vector<char> zeros (1024 * 1024, 0);
        for (int i = 0; i < 51; ++i)
            out.write (zeros.data(), zeros.size());
    }
    check (store.importImages ({ huge }, error) == 0 && error.contains ("50 MB"), "file over 50 MB is rejected");

    // mixed batch: good ones still go in
    auto good = incoming.getChildFile ("good.png");
    writeBytes (good, ImageFixtures::png, sizeof (ImageFixtures::png));
    check (store.importImages ({ fake, good, empty }, error) == 1 && error.contains ("fake.png") && error.contains ("empty.png") && ! error.contains ("good.png"),
           "mixed batch imports the valid file and reports the others");

    // unique, safe names
    check (store.importImages ({ good }, error) == 1 && user.getChildFile ("good-2.png").existsAsFile(), "same name twice -> good-2.png");
    check (store.importImages ({ good }, error) == 1 && user.getChildFile ("good-3.png").existsAsFile(), "third time -> good-3.png");
    auto odd = incoming.getChildFile ("we;ird $(name)`x`.png");
    writeBytes (odd, ImageFixtures::png, sizeof (ImageFixtures::png));
    auto hidden = incoming.getChildFile (".hidden.png");
    writeBytes (hidden, ImageFixtures::png, sizeof (ImageFixtures::png));
    auto dots = incoming.getChildFile ("....png");
    writeBytes (dots, ImageFixtures::png, sizeof (ImageFixtures::png));
    check (store.importImages ({ odd, hidden, dots }, error) == 3, "odd names import");
    bool safeNames = true;
    for (auto& f : user.findChildFiles (juce::File::findFiles, false, "*"))
    {
        const auto name = f.getFileName();
        safeNames &= ! name.startsWith (".") && f.getParentDirectory() == user;
        for (auto c : name.upToLastOccurrenceOf (".", false, false))
            safeNames &= juce::CharacterFunctions::isLetterOrDigit (c) || c == ' ' || c == '-' || c == '_';
    }
    check (safeNames && user.getChildFile ("weird namex.png").existsAsFile() && user.getChildFile ("hidden.png").existsAsFile()
           && user.getChildFile ("image.png").existsAsFile(), "stored names are sanitised, never dotfiles, never empty");
    auto longName = incoming.getChildFile (juce::String::repeatedString ("a", 150) + ".png");   // keeps the path under Windows MAX_PATH
    writeBytes (longName, ImageFixtures::png, sizeof (ImageFixtures::png));
    check (store.importImages ({ longName }, error) == 1 && user.getChildFile (juce::String::repeatedString ("a", 60) + ".png").existsAsFile(), "long names are truncated");

   #if ! JUCE_WINDOWS   // creating symlinks on Windows needs admin rights or developer mode
    // symlinked source resolves; directory symlink is not a file
    auto linkToImage = incoming.getChildFile ("link.png");
    ::symlink (good.getFullPathName().toRawUTF8(), linkToImage.getFullPathName().toRawUTF8());
    check (store.importImages ({ linkToImage }, error) == 1 && user.getChildFile ("link.png").existsAsFile()
           && ! user.getChildFile ("link.png").isSymbolicLink(), "symlinked source imports as a real copy");
   #endif
    check (! hasLeftovers (user), "no temp files left behind");

    // removal
    auto target = user.getChildFile ("good.png");
    check (store.removeImage (target) && ! target.exists(), "remove deletes a user picture");
    check (! store.removeImage (target), "removing it again fails quietly");
    auto bundledFile = bundled.getChildFile ("01-bundled.png");
    check (! store.removeImage (bundledFile) && bundledFile.existsAsFile(), "bundled picture can't be removed");
    auto precious = outside.getChildFile ("precious.png");
    writeBytes (precious, ImageFixtures::png, sizeof (ImageFixtures::png));
    check (! store.removeImage (precious) && precious.existsAsFile(), "file outside the folder can't be removed");
    check (! store.removeImage (user.getChildFile ("../outside/precious.png")) && precious.existsAsFile(), "path traversal with .. is refused");
    check (! store.removeImage (juce::File (user.getFullPathName() + "/../outside/precious.png")) && precious.existsAsFile(), "raw .. path is refused");
   #if ! JUCE_WINDOWS
    ::symlink (outside.getFullPathName().toRawUTF8(), user.getChildFile ("escape").getFullPathName().toRawUTF8());
    check (! store.removeImage (user.getChildFile ("escape/precious.png")) && precious.existsAsFile(), "file reached through a directory symlink is refused");
    check (! store.removeImage (user.getChildFile ("escape")) && outside.isDirectory(), "directory symlink itself is not removable as a picture");
    user.getChildFile ("escape").deleteFile();
    ::symlink (precious.getFullPathName().toRawUTF8(), user.getChildFile ("shortcut.png").getFullPathName().toRawUTF8());
    check (store.removeImage (user.getChildFile ("shortcut.png")) && precious.existsAsFile() && ! user.getChildFile ("shortcut.png").isSymbolicLink(),
           "a symlink inside the folder is removed, its target survives");
   #endif
    check (! store.removeImage (user) && user.isDirectory() && ! store.removeImage (user.getParentDirectory()), "folders are never removed");
    check (! store.removeImage ({}), "empty path refused");

   #if ! JUCE_WINDOWS
    // store with a symlinked user folder still works
    auto realUser = root.getChildFile ("real-user");
    realUser.createDirectory();
    auto linkedUser = root.getChildFile ("linked-user");
    ::symlink (realUser.getFullPathName().toRawUTF8(), linkedUser.getFullPathName().toRawUTF8());
    GalleryStore linked (linkedUser, bundled);
    check (linked.importImages ({ good }, error) == 1 && realUser.getChildFile ("good.png").existsAsFile()
           && linked.removeImage (linkedUser.getChildFile ("good.png")), "symlinked gallery folder: import and remove work");
   #endif

    // folder cannot be created
    auto blocker = root.getChildFile ("blocker");
    blocker.replaceWithText ("file in the way");
    GalleryStore broken (blocker.getChildFile ("gallery"), bundled);
    check (broken.importImages ({ good }, error) == 0 && error.isNotEmpty(), "unwritable gallery folder reports an error");
    check (store.importImages ({}, error) == 0 && error.isEmpty(), "importing nothing is not an error");

    root.deleteRecursively();
}

// ---- app prefs

void testAppPrefs()
{
    std::printf ("app prefs\n");
    {
        auto data = makeTempDir ("datadir");
        setEnvironment ("OSCILLA_DATA_DIR", data.getFullPathName());
        check (AppPrefs::defaultFile() == data.getChildFile ("prefs.json") && ProfileStore::defaultDirectory() == data.getChildFile ("profiles")
               && GalleryStore::defaultUserDirectory() == data.getChildFile ("Gallery"), "OSCILLA_DATA_DIR redirects prefs, profiles and gallery");
        setEnvironment ("OSCILLA_DATA_DIR", "relative/path");
        check (AppPrefs::defaultFile() == AppPaths::defaultDataDirectory().getChildFile ("prefs.json"), "relative OSCILLA_DATA_DIR is ignored");
        setEnvironment ("OSCILLA_DATA_DIR", {});
        check (ProfileStore::defaultDirectory() == AppPaths::defaultDataDirectory().getChildFile ("profiles"), "default location restored");
       #if JUCE_MAC
        check (AppPaths::defaultDataDirectory().getFullPathName().endsWith ("Library/Application Support/Oscilla"), "macOS data folder unchanged");
       #elif JUCE_WINDOWS
        check (AppPaths::defaultDataDirectory() == juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("OscillaFX"),
               "Windows data folder is %APPDATA%\\OscillaFX");
       #endif
        data.deleteRecursively();
    }
    auto dir = makeTempDir ("prefs");
    auto file = dir.getChildFile ("nested/prefs.json");
    {
        AppPrefs prefs (file);
        check (! prefs.gallerySlideshow() && prefs.gallerySlideshowSeconds() == 20 && ! prefs.firstRunDone() && ! file.exists(),
               "defaults: slideshow off, 20 s, first run pending; nothing written yet");
        prefs.setGallerySlideshow (true);
        prefs.setGallerySlideshowSeconds (45);
        prefs.setFirstRunDone (true);
        check (file.existsAsFile(), "written on change (parent folders created)");
    }
    {
        AppPrefs prefs (file);
        check (prefs.gallerySlideshow() && prefs.gallerySlideshowSeconds() == 45 && prefs.firstRunDone(), "values survive restart");
        prefs.setGallerySlideshowSeconds (1);
        check (prefs.gallerySlideshowSeconds() == AppPrefs::slideshowMinSeconds, "seconds clamped low");
        prefs.setGallerySlideshowSeconds (100000);
        check (prefs.gallerySlideshowSeconds() == AppPrefs::slideshowMaxSeconds, "seconds clamped high");
    }
    file.replaceWithText (R"({"gallerySlideshowSeconds": -50, "firstRunDone": true})");
    {
        AppPrefs prefs (file);
        check (prefs.gallerySlideshowSeconds() == AppPrefs::slideshowMinSeconds && prefs.firstRunDone() && ! prefs.gallerySlideshow(),
               "hand-edited file: clamped, missing keys default");
    }
    file.replaceWithText ("{ not json");
    {
        AppPrefs prefs (file);
        check (! prefs.firstRunDone() && prefs.gallerySlideshowSeconds() == 20 && file.getSiblingFile ("prefs.json.bak").existsAsFile(),
               "corrupt file: defaults, kept as .bak");
        prefs.setFirstRunDone (true);
        check (AppPrefs (file).firstRunDone(), "rewritten after corruption");
    }
    dir.deleteRecursively();
}

// ---- settings: night mode + limiter fields, preset safety

void testSafetySettings()
{
    std::printf ("night mode / limiter settings\n");
    DspSettings defaults;
    check (! defaults.nightMode && defaults.limiterCeilingDb == 0.0f && ! defaults.limiterEnabled(), "default off");

    DspSettings s;
    s.nightMode = true;
    s.limiterCeilingDb = -9.5f;
    check (DspSettings::fromVar (s.toVar()) == s, "json round trip");
    check (s != DspSettings {} && DspSettings::fromVar (s.toVar()).limiterEnabled(), "== sees both fields");
    auto old = DspSettings::fromVar (juce::JSON::parse (R"({"power": true, "preset": "Old", "bass": 4})"));
    check (! old.nightMode && old.limiterCeilingDb == 0.0f && near (old.bass, 4, 0), "profile from an older version loads with both off");
    auto wild = DspSettings::fromVar (juce::JSON::parse (R"({"limiterCeilingDb": -99, "nightMode": true})"));
    check (near (wild.limiterCeilingDb, -24, 0) && wild.nightMode, "ceiling clamped to -24");
    check (near (DspSettings::fromVar (juce::JSON::parse (R"({"limiterCeilingDb": 6})")).limiterCeilingDb, 0, 0), "ceiling above 0 clamped to 0 (off)");
    DspSettings nearZero;
    nearZero.limiterCeilingDb = -0.03f;
    check (! nearZero.limiterEnabled(), "-0.03 dB counts as off");

    {
        DspSettings messy;
        messy.fidelity = 50.0f; messy.bass = -3.0f; messy.eqGainDb[1] = 99.0f; messy.eqFreqHz[2] = 1.0f;
        messy.masterGainDb = std::nanf (""); messy.balance = 7.0f; messy.limiterCeilingDb = -80.0f; messy.displayMode = 17; messy.galleryIndex = -4;
        messy.clampToRanges();
        check (near (messy.fidelity, 10, 0) && near (messy.bass, 0, 0) && near (messy.eqGainDb[1], 12, 0) && near (messy.eqFreqHz[2], 20, 0)
               && near (messy.masterGainDb, 0, 0) && near (messy.balance, 1, 0) && near (messy.limiterCeilingDb, -24, 0)
               && messy.displayMode == DspSettings::numDisplayModes - 1 && messy.galleryIndex == 0, "clampToRanges pulls everything into range, NaN to default");
        DspSettings fine = sampleSettings();
        auto copy = fine;
        copy.clampToRanges();
        check (copy == fine, "clampToRanges leaves valid settings alone");
    }

    // hand-off carries the fields untouched
    auto p = DspParams::fromSettings (s);
    check (p.nightMode && near (p.limiterCeilingDb, -9.5, 0), "DspParams carries night mode and ceiling");
    TripleBuffer<DspParams> box;
    box.publish (p);
    DspParams got;
    check (box.fetch (got) && got.nightMode && near (got.limiterCeilingDb, -9.5, 0), "fields cross the triple buffer");

    // night mode shapes the effective effects, not the stored ones
    DspSettings loud;
    loud.dynamicBoost = 1.0f;
    loud.bass = 9.0f;
    loud.eqGainDb[0] = 8.0f;    // 62.5 Hz
    loud.eqGainDb[2] = 5.0f;    // 214 Hz
    loud.eqGainDb[6] = 7.0f;    // 2.5 kHz
    auto plain = DspParams::fromSettings (loud);
    loud.nightMode = true;
    auto night = DspParams::fromSettings (loud);
    check (night.effects[DfxDsp::DynamicBoost] >= 6.0f && night.effects[DfxDsp::Bass] <= 3.0f, "night: Dynamic Boost raised, Bass capped");
    check (near (night.eqGainDb[0], 3, 0) && near (night.eqGainDb[2], 5, 0) && near (night.eqGainDb[6], 7, 0), "night: only boost below 200 Hz is capped");
    check (near (plain.effects[DfxDsp::Bass], 9, 0) && near (plain.eqGainDb[0], 8, 0), "night off: values untouched");
    loud.dynamicBoost = 9.0f;
    loud.bass = 1.0f;
    check (near (DspParams::fromSettings (loud).effects[DfxDsp::DynamicBoost], 9, 0) && near (DspParams::fromSettings (loud).effects[DfxDsp::Bass], 1, 0),
           "night never lowers Dynamic Boost or raises Bass");
    check (near (loud.dynamicBoost, 9, 0) && near (loud.bass, 1, 0), "stored settings unchanged by the derivation");
    loud.power = false;
    check (! DspParams::fromSettings (loud).nightMode, "power off: night mode inactive");
    loud.limiterCeilingDb = -6.0f;
    check (near (DspParams::fromSettings (loud).limiterCeilingDb, -6, 0), "power off: limiter setting still passes (safety net)");

    // CRITICAL: presets must never reset them
    DspSettings base;
    base.nightMode = true;
    base.limiterCeilingDb = -9.0f;
    base.masterGainDb = -2.0f;
    for (const char* name : { "Movies", "Music", "Pop" })
    {
        auto loaded = Presets::load (name, base);
        check (loaded && loaded->nightMode && near (loaded->limiterCeilingDb, -9, 0) && near (loaded->masterGainDb, -2, 0),
               juce::String ("preset ") + name + " keeps night mode, limiter, gain");
    }
}

// ---- dynamics

std::vector<float> sineBlock (double hz, double amplitude, double seconds, double rate = 48000.0, double phase0 = 0.0)
{
    std::vector<float> out ((size_t) (seconds * rate) * 2);
    for (size_t i = 0; i < out.size() / 2; ++i)
        out[2 * i] = out[2 * i + 1] = (float) (amplitude * std::sin (phase0 + 2 * M_PI * hz * (double) i / rate));
    return out;
}

std::vector<float> pinkNoise (double rmsDb, double seconds, unsigned seed, double rate = 48000.0)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> dist (0.0, 1.0);
    const size_t frames = (size_t) (seconds * rate);
    std::vector<float> out (frames * 2);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        const double w = dist (rng);
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        out[2 * i] = (float) v;
        out[2 * i + 1] = (float) (0.7 * v + 0.3 * dist (rng));
        sum += v * v;
    }
    const double gain = std::pow (10.0, rmsDb / 20.0) / std::sqrt (sum / (double) frames);
    for (auto& v : out)
        v = (float) (v * gain);
    return out;
}

float peakOf (const std::vector<float>& x, size_t fromFrame = 0)
{
    float peak = 0;
    for (size_t i = 2 * fromFrame; i < x.size(); ++i)
        peak = std::max (peak, std::fabs (x[i]));
    return peak;
}

double toDbfs (double linear) { return 20.0 * std::log10 (linear + 1.0e-12); }

// Runs `signal` through a fresh chain in realistic callback sizes.
std::vector<float> runChain (Dynamics::DynamicsChain& chain, std::vector<float> signal, int block = 480)
{
    const int frames = (int) signal.size() / 2;
    for (int i = 0; i < frames; i += block)
        chain.process (signal.data() + 2 * i, std::min (block, frames - i));
    return signal;
}

void testDynamics()
{
    std::printf ("dynamics (night mode compressor, safety limiter)\n");
    constexpr double RATE = 48000.0;
    using Dynamics::DynamicsChain;
    using Dynamics::NightCompressor;

    {
        DynamicsChain chain;
        chain.prepare (RATE);
        auto signal = sineBlock (1000, 0.7, 1.0);
        auto out = runChain (chain, signal);
        check (out == signal && ! chain.isActive(), "off + no limiter: bit-exact bypass, chain idle");
    }

    check (near (NightCompressor::reductionDb (-40), 0, 1e-6) && NightCompressor::reductionDb (-24) < 0.6f
           && near (NightCompressor::reductionDb (0), 16.0, 1e-4), "static curve: nothing below the knee, 16 dB at 0 dBFS (3:1 from -24)");
    std::printf ("  makeup %.2f dB\n", NightCompressor::makeupDb());
    check (near (NightCompressor::makeupDb(), 4.0, 0.01), "auto makeup = reduction at -18 dBFS = 4 dB");

    // steady-state level -> level (night mode on, limiter off: safety ceiling -1 dBFS applies)
    double lastOutDb = 0;
    for (double inDb : { -50.0, -40.0, -30.0, -20.0, -12.0, -6.0, -3.0 })
    {
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (true, 0.0f);
        auto out = runChain (chain, sineBlock (1000, std::pow (10.0, inDb / 20.0), 3.0));
        const double outDb = toDbfs (peakOf (out, (size_t) (2.0 * RATE)));
        const double expected = inDb + NightCompressor::makeupDb() - NightCompressor::reductionDb ((float) inDb);
        std::printf ("  in %6.1f dBFS -> out %6.2f dBFS (curve %6.2f)\n", inDb, outDb, expected);
        check (near (outDb, expected, 0.7), "level " + juce::String (inDb, 0) + " dBFS follows the 3:1 curve with makeup");
        lastOutDb = outDb;
    }
    check (lastOutDb < -8.0, "loud input ends well below full scale");

    {   // 3:1 slope above the knee
        auto steady = [&] (double inDb)
        {
            DynamicsChain chain;
            chain.prepare (RATE);
            chain.configure (true, 0.0f);
            return toDbfs (peakOf (runChain (chain, sineBlock (1000, std::pow (10.0, inDb / 20.0), 3.0)), (size_t) (2.0 * RATE)));
        };
        check (near (steady (-6) - steady (-15), 3.0, 0.5), "9 dB more input -> 3 dB more output");
    }

    {   // attack and release time constants (5 ms / 200 ms), read from the gain reduction state
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (true, 0.0f);
        const float finalReduction = NightCompressor::reductionDb (-6.0f);
        auto burst = sineBlock (1000, 0.5, 0.005);
        runChain (chain, burst, 48);
        const float afterAttack = chain.nightCompressor().currentReductionDb();
        std::printf ("  reduction after 5 ms: %.2f of %.2f dB\n", afterAttack, finalReduction);
        check (afterAttack > 0.45f * finalReduction && afterAttack < 0.85f * finalReduction, "attack ~5 ms: about 63 percent of the way after 5 ms");
        runChain (chain, sineBlock (1000, 0.5, 1.0));
        check (near (chain.nightCompressor().currentReductionDb(), finalReduction, 1.0), "settles at the static reduction");
        const float held = chain.nightCompressor().currentReductionDb();
        runChain (chain, std::vector<float> ((size_t) (0.2 * RATE) * 2, 0.0f));
        const float afterRelease = chain.nightCompressor().currentReductionDb();
        std::printf ("  reduction 200 ms after the signal stops: %.2f of %.2f dB\n", afterRelease, held);
        check (afterRelease > 0.25f * held && afterRelease < 0.5f * held, "release ~200 ms: about 37 percent left after 200 ms");
    }

    {   // never clips: hot pink noise, loud sine bursts, +6 dBFS sine
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (true, 0.0f);
        const float ceiling = std::pow (10.0f, Dynamics::NIGHT_SAFETY_CEILING_DB / 20.0f);
        auto pink = runChain (chain, pinkNoise (-3.0, 10.0, 1));
        check (peakOf (pink) <= ceiling, "night: very hot pink noise stays under the -1 dBFS safety ceiling");
        auto onset = sineBlock (200, 4.0, 0.2); // +12 dBFS right after silence: the compressor attack is too slow for it
        onset.insert (onset.begin(), (size_t) (0.5 * RATE) * 2, 0.0f);
        DynamicsChain fresh;
        fresh.prepare (RATE);
        fresh.configure (true, 0.0f);
        check (peakOf (runChain (fresh, onset)) <= ceiling, "night: loud onset out of silence cannot clip");
        DynamicsChain bursts;
        bursts.prepare (RATE);
        bursts.configure (true, 0.0f);
        auto train = sineBlock (80, 0.02, 0.3);
        for (int i = 0; i < 6; ++i)
        {
            auto loudBurst = sineBlock (3000, 2.0, 0.05);
            train.insert (train.end(), loudBurst.begin(), loudBurst.end());
            auto quiet = sineBlock (80, 0.02, 0.4);
            train.insert (train.end(), quiet.begin(), quiet.end());
        }
        check (peakOf (runChain (bursts, train)) <= ceiling, "night: burst train never exceeds the ceiling");
    }

    // limiter: ceilings, hot noise, sweeps, transparency below the ceiling
    for (float ceilingDb : { -1.0f, -6.0f, -12.0f, -24.0f })
    {
        const float ceiling = std::pow (10.0f, ceilingDb / 20.0f);
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (false, ceilingDb);
        auto noise = runChain (chain, pinkNoise (-4.0, 12.0, 7));
        const float noisePeak = peakOf (noise);
        check (noisePeak <= ceiling, "limiter " + juce::String (ceilingDb, 0) + " dB: loud pink noise never exceeds the ceiling");
        check (toDbfs (noisePeak) > ceilingDb - 2.5, "limiter " + juce::String (ceilingDb, 0) + " dB: limits, does not mute (peaks within 2.5 dB of the ceiling)");

        DynamicsChain sweepChain;
        sweepChain.prepare (RATE);
        sweepChain.configure (false, ceilingDb);
        const int sweepFrames = (int) (6.0 * RATE);
        std::vector<float> sweep ((size_t) sweepFrames * 2);
        double phase = 0;
        for (int i = 0; i < sweepFrames; ++i)
        {
            const double t = (double) i / sweepFrames;
            const double hz = 20.0 * std::pow (1000.0, t);   // 20 Hz .. 20 kHz
            phase += 2 * M_PI * hz / RATE;
            sweep[(size_t) (2 * i)] = sweep[(size_t) (2 * i + 1)] = (float) (2.0 * std::sin (phase)); // +6 dBFS
        }
        check (peakOf (runChain (sweepChain, sweep, 256)) <= ceiling, "limiter " + juce::String (ceilingDb, 0) + " dB: +6 dBFS sine sweep 20 Hz..20 kHz never exceeds the ceiling");
    }
    {
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (false, -6.0f);
        auto quiet = sineBlock (440, 0.1, 1.0);  // -20 dBFS, well below -6 dB
        check (runChain (chain, quiet) == quiet, "limiter leaves signal below the ceiling bit-exact");
    }
    {   // stereo: a loud left channel must not let the right one through un-limited, and vice versa
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (false, -6.0f);
        auto signal = sineBlock (300, 1.5, 1.0);
        for (size_t i = 0; i < signal.size() / 2; ++i)
            signal[2 * i + 1] *= 0.1f;
        const float ceiling = std::pow (10.0f, -6.0f / 20.0f);
        auto out = runChain (chain, signal);
        check (peakOf (out) <= ceiling, "limiter: asymmetric stereo stays under the ceiling");
    }
    {   // release is smooth: after a loud burst, gain returns gradually
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (false, -12.0f);
        auto burst = sineBlock (500, 1.0, 0.05);
        auto tail = sineBlock (500, 0.1, 0.5);
        burst.insert (burst.end(), tail.begin(), tail.end());
        auto out = runChain (chain, burst, 128);
        double maxStep = 0, prev = 0;
        for (size_t i = (size_t) (0.06 * RATE); i < out.size() / 2; ++i)
        {
            // gain estimate: output peak per cycle / 0.1
            if (i % 96 == 0)
            {
                float localPeak = 0;
                for (size_t k = i; k < i + 96 && 2 * k < out.size(); ++k)
                    localPeak = std::max (localPeak, std::fabs (out[2 * k]));
                const double gain = localPeak / 0.1;
                if (prev > 0)
                    maxStep = std::max (maxStep, std::fabs (gain - prev));
                prev = gain;
            }
        }
        std::printf ("  limiter release: largest gain change per 2 ms after the burst %.3f\n", maxStep);
        check (maxStep < 0.12, "limiter release is gradual (no pumping steps)");
    }

    {   // click-free enabling and disabling
        DynamicsChain chain;
        chain.prepare (RATE);
        auto signal = sineBlock (440, 0.5, 3.0);
        const int frames = (int) signal.size() / 2;
        bool night = false;
        for (int i = 0; i < frames; i += 480)
        {
            if ((i / 480) % 100 == 50)
            {
                night = ! night;
                chain.configure (night, 0.0f);
            }
            chain.process (signal.data() + 2 * i, std::min (480, frames - i));
        }
        double maxStep = 0;
        for (int i = 1; i < frames; ++i)
            maxStep = std::max (maxStep, (double) std::fabs (signal[(size_t) (2 * i)] - signal[(size_t) (2 * (i - 1))]));
        const double naturalStep = 0.5 * 2 * M_PI * 440.0 / RATE;
        std::printf ("  toggling night mode: max sample step %.4f (sine alone %.4f)\n", maxStep, naturalStep);
        check (maxStep < 1.5 * naturalStep, "night mode on/off is click-free");
    }
    {   // switching the limiter off hands back unity gain without a jump
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (false, -12.0f);
        runChain (chain, sineBlock (500, 1.0, 0.2));
        check (chain.safetyLimiter().currentGain() < 0.5f, "limiter holding gain reduction");
        chain.configure (false, 0.0f);
        auto tail = runChain (chain, sineBlock (500, 0.1, 2.0));
        check (near (tail[tail.size() - 2], 0.1 * std::sin (2 * M_PI * 500.0 * (double) (tail.size() / 2 - 1) / RATE), 1e-3) && ! chain.isActive(),
               "limiter switched off: gain recovers, chain goes idle");
    }
    {   // a DSP fault (NaN / inf) is silenced and must not poison the smoothing state
        for (bool night : { false, true })
        {
            DynamicsChain chain;
            chain.prepare (RATE);
            chain.configure (night, -6.0f);
            auto signal = sineBlock (440, 0.3, 1.0);
            const float nan = std::nanf (""), inf = INFINITY;
            signal[2000] = nan;
            signal[4001] = nan;      // right channel only
            signal[6000] = inf;
            signal[8001] = -inf;
            signal[10000] = 5.0e8f;
            auto out = runChain (chain, signal);
            bool finite = true;
            for (float v : out)
                finite &= std::isfinite (v);
            auto after = sineBlock (440, 0.3, 0.5);
            auto recovered = runChain (chain, after);
            check (finite && std::isfinite (chain.nightCompressor().currentReductionDb()) && std::isfinite (chain.safetyLimiter().currentGain())
                       && peakOf (recovered, (size_t) (0.25 * RATE)) > 0.1f,
                   night ? "night + limiter: NaN/inf silenced, signal continues" : "limiter: NaN/inf silenced, signal continues");
        }
    }
    {   // sample-rate independence
        for (double rate : { 44100.0, 96000.0 })
        {
            DynamicsChain chain;
            chain.prepare (rate);
            chain.configure (true, -6.0f);
            auto out = runChain (chain, pinkNoise (-6.0, 4.0, 3, rate), 512);
            check (peakOf (out) <= std::pow (10.0f, -6.0f / 20.0f), "limiter at " + juce::String (rate, 0) + " Hz holds the ceiling");
        }
    }
    {   // user ceiling above the night safety ceiling: the lower one wins; a lower user ceiling is respected
        DynamicsChain chain;
        chain.prepare (RATE);
        chain.configure (true, -3.0f);
        check (peakOf (runChain (chain, pinkNoise (-3.0, 4.0, 5))) <= std::pow (10.0f, -3.0f / 20.0f), "night + -3 dB limiter: -3 dB ceiling holds");
    }
}

// ---- Windows audio backend: format conversion, channel maps, endpoint classifier, default roles

#if JUCE_WINDOWS
void testWasapiFormat()
{
    std::printf ("wasapi formats\n");
    using namespace WasapiFormat;
    check (describe (true, false, 32, 2, 48000, 3).type == SampleType::float32, "float32 recognised");
    check (describe (false, true, 16, 2, 44100, 0).type == SampleType::int16, "int16 recognised");
    check (describe (false, true, 24, 2, 48000, 0).type == SampleType::int24 && describe (false, true, 24, 2, 48000, 0).bytesPerFrame() == 6,
           "packed int24, 6 bytes per stereo frame");
    check (describe (false, true, 32, 8, 96000, 0x63f).type == SampleType::int32, "int32 / 24-in-32 recognised");
    check (describe (false, true, 8, 2, 48000, 0).type == SampleType::unsupported && ! describe (false, true, 8, 2, 48000, 0).isUsable(),
           "8-bit rejected");

    uint8_t bytes[4] {};
    writeSample (bytes, SampleType::int16, 0.5f);
    check (near (readSample (bytes, SampleType::int16), 0.5, 1.0 / 32768), "int16 round trip");
    writeSample (bytes, SampleType::int16, 2.0f);
    check (bytes[0] == 0xff && bytes[1] == 0x7f, "int16 clamps at +32767");
    writeSample (bytes, SampleType::int24, -1.0f);
    check (near (readSample (bytes, SampleType::int24), -1.0, 1.0 / 8388608) && bytes[2] == 0x80, "int24 negative full scale, sign extended");
    writeSample (bytes, SampleType::int24, 0.25f);
    check (near (readSample (bytes, SampleType::int24), 0.25, 1.0 / 8388608), "int24 round trip");
    writeSample (bytes, SampleType::int32, 1.0f);
    check (near (readSample (bytes, SampleType::int32), 1.0, 1e-6), "int32 full scale");
    writeSample (bytes, SampleType::float32, -0.75f);
    check (near (readSample (bytes, SampleType::float32), -0.75, 0), "float32 copied bit-exact");
}

void testStereoMaps()
{
    std::printf ("wasapi channel maps\n");
    using namespace WasapiFormat;
    const auto mono = stereoMapFor (1, 0x4);
    check (mono.mono && mono.left == 0 && mono.right == 0, "mono endpoint");
    const auto stereo = stereoMapFor (2, 0x3);
    check (! stereo.mono && stereo.left == 0 && stereo.right == 1 && stereo.rearLeft < 0, "stereo");
    const auto surround51 = stereoMapFor (6, 0x3f);
    check (surround51.left == 0 && surround51.right == 1 && surround51.rearLeft < 0 && surround51.rearRight < 0, "5.1: front pair only");
    const auto surround71 = stereoMapFor (8, 0x63f);
    check (surround71.left == 0 && surround71.right == 1, "7.1: front pair");
    const auto quad = stereoMapFor (4, 0x33);
    check (quad.left == 0 && quad.right == 1 && quad.rearLeft == 2 && quad.rearRight == 3, "quad: copied to the back pair");
    const auto quadNoMask = stereoMapFor (4, 0);
    check (quadNoMask.rearLeft == 2 && quadNoMask.rearRight == 3, "quad without mask");
    const auto noFront = stereoMapFor (2, 0x30);
    check (noFront.left == 0 && noFront.right == 1, "mask without front pair falls back to 0 / 1");
    const auto noLeft = stereoMapFor (3, 0x2 | 0x4 | 0x8);
    check (noLeft.left == 0 && noLeft.right == 1, "mask without front left: 0 / 1, never the same channel twice");

    // stereo -> 5.1 int16: only FL / FR carry signal
    const float stereoIn[] = { 0.5f, -0.5f, 0.25f, -0.25f };
    const auto six = describe (false, true, 16, 6, 48000, 0x3f);
    int16_t out6[12];
    std::fill (std::begin (out6), std::end (out6), (int16_t) 1234);
    fromStereo (stereoIn, six, surround51, reinterpret_cast<uint8_t*> (out6), 2);
    bool othersSilent = true;
    for (int frame = 0; frame < 2; ++frame)
        for (int ch = 2; ch < 6; ++ch)
            othersSilent &= out6[frame * 6 + ch] == 0;
    check (out6[0] == 16384 && out6[1] == -16384 && out6[6] == 8192 && othersSilent, "stereo -> 5.1 int16: front pair, rest silent");

    const auto monoFloat = describe (true, false, 32, 1, 48000, 0);
    float outMono[2];
    fromStereo (stereoIn, monoFloat, mono, reinterpret_cast<uint8_t*> (outMono), 2);
    check (near (outMono[0], 0, 0) && near (outMono[1], 0, 0), "mono gets (L + R) / 2");

    const auto quadFloat = describe (true, false, 32, 4, 48000, 0x33);
    float outQuad[8];
    fromStereo (stereoIn, quadFloat, quad, reinterpret_cast<uint8_t*> (outQuad), 2);
    check (near (outQuad[2], 0.5, 0) && near (outQuad[3], -0.5, 0) && near (outQuad[6], 0.25, 0), "quad back pair mirrors the front");

    // 5.1 float capture -> stereo picks FL / FR, mono capture is duplicated
    const float six51[] = { 0.1f, 0.2f, 0.9f, 0.9f, 0.9f, 0.9f, 0.3f, 0.4f, 0.9f, 0.9f, 0.9f, 0.9f };
    float captured[4];
    toStereo (reinterpret_cast<const uint8_t*> (six51), describe (true, false, 32, 6, 48000, 0x3f), surround51, captured, 2);
    check (near (captured[0], 0.1f, 0) && near (captured[1], 0.2f, 0) && near (captured[2], 0.3f, 0) && near (captured[3], 0.4f, 0),
           "5.1 capture -> front pair");
    const int16_t monoIn[] = { 16384, -8192 };
    toStereo (reinterpret_cast<const uint8_t*> (monoIn), describe (false, true, 16, 1, 48000, 0), mono, captured, 2);
    check (near (captured[0], 0.5, 0) && near (captured[1], 0.5, 0) && near (captured[2], -0.25, 0) && near (captured[3], -0.25, 0),
           "mono capture duplicated");
}

void testEndpointTraits()
{
    std::printf ("windows endpoint classifier\n");
    using namespace EndpointTraits;
    struct Case
    {
        const char* label; const char* name; const char* adapter; const char* enumerator;
        int formFactor; bool battery; DeviceKind expected;
    };
    const Case cases[] = {
        { "laptop speakers",        "Speakers (Realtek(R) Audio)",           "Realtek(R) Audio",      "HDAUDIO",    speakers,   true,  DeviceKind::builtInSpeakers },
        { "SST laptop speakers",    "Speakers (Intel Smart Sound)",          "Intel Smart Sound",     "INTELAUDIO", speakers,   true,  DeviceKind::builtInSpeakers },
        { "desktop rear jack",      "Speakers (Realtek(R) Audio)",           "Realtek(R) Audio",      "HDAUDIO",    speakers,   false, DeviceKind::externalSpeakers },
        { "headphone jack",         "Headphones (Realtek(R) Audio)",         "Realtek(R) Audio",      "HDAUDIO",    headphones, true,  DeviceKind::headphonesWired },
        { "combo jack headset",     "Headset (Realtek(R) Audio)",            "Realtek(R) Audio",      "HDAUDIO",    headset,    true,  DeviceKind::headphonesWired },
        { "S/PDIF out",             "Realtek Digital Output (Realtek(R) Audio)", "Realtek(R) Audio",  "HDAUDIO",    spdif,      false, DeviceKind::externalSpeakers },
        { "line out",               "Line (Realtek(R) Audio)",               "Realtek(R) Audio",      "HDAUDIO",    lineLevel,  false, DeviceKind::externalSpeakers },
        { "HDMI monitor",           "LG ULTRAGEAR (NVIDIA High Definition Audio)", "NVIDIA High Definition Audio", "HDAUDIO", digitalAudioDisplayDevice, false, DeviceKind::display },
        { "BT headphones",          "Headphones (WH-1000XM5)",               "WH-1000XM5",            "BTHENUM",    headphones, true,  DeviceKind::headphonesBluetooth },
        { "AirPods A2DP",           "Headphones (Efti's AirPods Pro)",       "Efti's AirPods Pro",    "BTHENUM",    headphones, true,  DeviceKind::airPods },
        { "AirPods hands-free",     "Headset (AirPods Pro Hands-Free AG Audio)", "AirPods Pro Hands-Free AG Audio", "BTHHFENUM", headset, true, DeviceKind::airPods },
        { "BT speaker",             "Speakers (JBL Flip 6)",                 "JBL Flip 6",            "BTHENUM",    speakers,   true,  DeviceKind::externalSpeakers },
        { "USB headset as speakers","Speakers (Logitech G Pro X)",           "Logitech G Pro X",      "USB",        speakers,   true,  DeviceKind::headphonesUsb },
        { "USB headset",            "Headset Earphone (HyperX Cloud)",       "HyperX Cloud",          "USB",        headset,    true,  DeviceKind::headphonesUsb },
        { "USB display",            "Speakers (Studio Display)",             "Studio Display",        "USB",        speakers,   false, DeviceKind::display },
        { "USB monitors",           "Speakers (KRK Monitor Interface)",      "KRK Monitor Interface", "USB",        speakers,   false, DeviceKind::externalSpeakers },
        { "USB DAC",                "Speakers (USB Audio DAC)",              "USB Audio DAC",         "USB",        speakers,   true,  DeviceKind::headphonesUsb },
        { "virtual cable",          "CABLE Input (VB-Audio Virtual Cable)",  "VB-Audio Virtual Cable","ROOT",       speakers,   true,  DeviceKind::unknown },
        { "unknown bus, AirPods",   "AirPods Pro",                           "",                      "",           -1,         true,  DeviceKind::airPods },
    };
    for (const auto& c : cases)
    {
        Facts facts;
        facts.uid = "{0.0.0.00000000}.{test}";
        facts.name = c.name;
        facts.adapterName = c.adapter;
        facts.enumerator = c.enumerator;
        facts.formFactor = c.formFactor;
        facts.hasSystemBattery = c.battery;
        const auto got = DeviceKinds::classify (traitsFor (facts));
        check (got == c.expected, juce::String (c.label) + " -> " + DeviceKinds::id (c.expected)
                                      + (got == c.expected ? "" : " (got " + DeviceKinds::id (got) + ")"));
    }
}

struct FakeEndpoints : DefaultRoles::Endpoints
{
    std::array<juce::String, DefaultRoles::numRoles> defaults;
    juce::StringArray active { "spk", "hp", "headset", "fx" };

    juce::String defaultFor (DefaultRoles::Role role) const override { return defaults[(size_t) role]; }
    bool setDefault (const juce::String& id, DefaultRoles::Role role) override { defaults[(size_t) role] = id; return true; }
    bool isActive (const juce::String& id) const override { return active.contains (id); }

    bool all (const juce::String& id) const { return defaults[0] == id && defaults[1] == id && defaults[2] == id; }
};

void testDefaultRoles()
{
    std::printf ("default playback roles\n");
    using namespace DefaultRoles;
    {
        FakeEndpoints endpoints;
        endpoints.defaults = { "spk", "spk", "spk" };
        Takeover takeover;
        check (takeover.begin (endpoints, "fx") && endpoints.all ("fx"), "single-device setup: every role moves to the virtual endpoint");
        check (takeover.original (console) == "spk" && takeover.holds (communications), "originals remembered");
        takeover.restore (endpoints, "hp");
        check (endpoints.all ("hp") && ! takeover.isActive(), "stop: every role goes to the output being heard");
    }
    {
        FakeEndpoints endpoints;
        endpoints.defaults = { "spk", "spk", "headset" };
        Takeover takeover;
        takeover.begin (endpoints, "fx");
        check (endpoints.defaults[0] == "fx" && endpoints.defaults[1] == "fx" && endpoints.defaults[2] == "headset",
               "separate call headset left alone");
        takeover.restore (endpoints, "spk");
        check (endpoints.defaults[0] == "spk" && endpoints.defaults[1] == "spk" && endpoints.defaults[2] == "headset", "restore keeps the headset");
    }
    {
        FakeEndpoints endpoints;
        endpoints.defaults = { "spk", "spk", "spk" };
        Takeover takeover;
        takeover.begin (endpoints, "fx");
        endpoints.defaults[0] = "hp";
        takeover.userChose (endpoints, console, "fx");
        check (endpoints.defaults[0] == "fx" && takeover.holds (console), "user picks a device: console pointed back at the virtual endpoint");
        endpoints.defaults[2] = "headset";
        takeover.userChose (endpoints, communications, "fx");
        check (endpoints.defaults[2] == "headset" && ! takeover.holds (communications), "user moves communications: given up");
        takeover.restore (endpoints, "hp");
        check (endpoints.defaults[0] == "hp" && endpoints.defaults[1] == "hp" && endpoints.defaults[2] == "headset", "restore skips released role");
    }
    {
        FakeEndpoints endpoints;
        endpoints.defaults = { "spk", "spk", "spk" };
        Takeover takeover;
        takeover.begin (endpoints, "fx");
        endpoints.active.removeString ("hp");
        takeover.restore (endpoints, "hp");
        check (endpoints.all ("spk"), "output gone: original endpoints restored");
    }
    {
        FakeEndpoints endpoints;
        endpoints.defaults = { "fx", "fx", "fx" };   // left behind by a crash
        Takeover takeover;
        takeover.begin (endpoints, "fx");
        check (takeover.original (console).isEmpty() && takeover.holds (communications), "virtual endpoint is never remembered as original");
        takeover.restore (endpoints, "spk");
        check (endpoints.all ("spk"), "after a crash: restored to the output");
    }
    {
        FakeEndpoints endpoints;
        endpoints.defaults = { "spk", "spk", "spk" };
        Takeover takeover;
        takeover.begin (endpoints, "fx");
        takeover.begin (endpoints, "fx");
        check (takeover.original (console) == "spk", "begin again keeps the first originals");
        endpoints.defaults[1] = "hp";   // moved by another app meanwhile
        takeover.restore (endpoints, "spk");
        check (endpoints.defaults[1] == "hp" && endpoints.defaults[0] == "spk", "role changed by someone else is left alone");
    }
}
#endif

// ---- Controller with temp locations (never starts audio)

struct CountingListener : public juce::ChangeListener
{
    void changeListenerCallback (juce::ChangeBroadcaster*) override { ++count; }
    int count = 0;
};

void pumpMessages() { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); }

void testControllerApi()
{
    std::printf ("controller api (temp folders, audio never started)\n");
    auto root = makeTempDir ("controller");
    Controller::Locations where;
    where.profilesDirectory = root.getChildFile ("profiles");
    where.prefsFile = root.getChildFile ("prefs.json");
    where.galleryDirectory = root.getChildFile ("gallery");
    where.bundledGalleryDirectory = root.getChildFile ("bundled");

    {
        Controller controller (where);
        CountingListener listener;
        controller.addChangeListener (&listener);
        const auto device = controller.activeDevice();
        if (device.uid.isEmpty())
            std::printf ("  (no output device on this machine: suggestion checks skipped)\n");
        else
        {
            auto pending = controller.pendingSuggestion();
            check (pending && pending->deviceUid == device.uid && pending->options.size() == 5, "first launch: suggestion pending for the active device");
            check (controller.settings() == DspSettings {}, "settings untouched while a suggestion is pending");
            std::printf ("  active device \"%s\" classified as %s\n", device.name.toRawUTF8(), pending ? pending->deviceKind.toRawUTF8() : "?");

            controller.applySuggestion ("nonsense");
            check (controller.pendingSuggestion().has_value() && controller.settings() == DspSettings {}, "unknown starter id ignored");
            controller.update ([] (DspSettings& s) { s.nightMode = true; s.limiterCeilingDb = -10.0f; s.masterGainDb = -3.0f; });
            check (! controller.pendingSuggestion(), "a manual edit ends the suggestion");
            pumpMessages();
            check (listener.count > 0, "change message fired");
        }
        controller.removeChangeListener (&listener);
    }
    where.profilesDirectory.deleteRecursively();
    {
        Controller controller (where);
        if (controller.activeDevice().uid.isNotEmpty())
        {
            CountingListener listener;
            controller.addChangeListener (&listener);
            check (controller.pendingSuggestion().has_value(), "fresh profile folder: pending again");
            controller.applySuggestion ("voice");
            pumpMessages();
            check (! controller.pendingSuggestion() && controller.settings().presetName == "Voice", "apply: starter applied, suggestion closed");
            check (listener.count > 0, "apply fires a change message");
            controller.update ([] (DspSettings& s) { s.nightMode = true; s.limiterCeilingDb = -10.0f; });
            controller.loadPreset ("Movies");
            check (controller.settings().nightMode && near (controller.settings().limiterCeilingDb, -10, 0) && controller.settings().presetName == "Movies",
                   "Controller::loadPreset keeps night mode and limiter");
            controller.removeChangeListener (&listener);
        }
    }
    {
        Controller controller (where);
        if (controller.activeDevice().uid.isNotEmpty())
        {
            check (! controller.pendingSuggestion(), "after apply and restart: no suggestion");
            check (controller.settings().presetName == "Movies", "settings restored on restart");
        }
    }
    where.profilesDirectory.deleteRecursively();
    {
        Controller controller (where);
        if (controller.activeDevice().uid.isNotEmpty())
        {
            CountingListener listener;
            controller.addChangeListener (&listener);
            controller.dismissSuggestion (false);
            pumpMessages();
            check (! controller.pendingSuggestion() && controller.settings() == DspSettings {} && listener.count > 0, "dismiss: closed, settings unchanged, notified");
            controller.removeChangeListener (&listener);
        }
    }
    {
        Controller controller (where);
        if (controller.activeDevice().uid.isNotEmpty())
        {
            check (controller.pendingSuggestion().has_value(), "dismissed without 'don't ask': back after restart");
            controller.dismissSuggestion (true);
        }
    }
    {
        Controller controller (where);
        check (! controller.pendingSuggestion(), "dismissed with 'don't ask': gone for good");
    }

    // gallery + prefs through the controller
    {
        Controller controller (where);
        CountingListener listener;
        controller.addChangeListener (&listener);
        auto incoming = root.getChildFile ("incoming");
        incoming.createDirectory();
        auto pic = incoming.getChildFile ("pic.png");
        writeBytes (pic, ImageFixtures::png, sizeof (ImageFixtures::png));
        auto notPic = incoming.getChildFile ("notes.png");
        notPic.replaceWithText ("hello");
        juce::String error;
        check (controller.importGalleryImages ({ pic, notPic }, error) == 1 && error.contains ("notes.png"), "controller imports the valid picture, reports the other");
        pumpMessages();
        check (listener.count > 0, "import fires a change message");
        auto images = controller.galleryImages();
        check (images.size() == 1 && images[0].getFileName() == "pic.png", "galleryImages lists it");
        check (controller.removeGalleryImage (images[0]) && controller.galleryImages().isEmpty(), "removeGalleryImage deletes it");
        check (! controller.removeGalleryImage (pic) && pic.existsAsFile(), "removeGalleryImage refuses files outside the gallery");

        check (! controller.gallerySlideshow() && controller.gallerySlideshowSeconds() == 20 && controller.firstRunPending(), "prefs defaults through the controller");
        listener.count = 0;
        controller.setGallerySlideshow (true);
        controller.setGallerySlideshowSeconds (30);
        controller.markFirstRunDone();
        pumpMessages();
        check (listener.count > 0 && ! controller.firstRunPending(), "prefs setters notify");
        controller.removeChangeListener (&listener);
    }
    {
        Controller controller (where);
        check (controller.gallerySlideshow() && controller.gallerySlideshowSeconds() == 30 && ! controller.firstRunPending(), "prefs survive restart");
    }
    root.deleteRecursively();
}
} // namespace

#if JUCE_WINDOWS
// WebP / HEIC need the Microsoft Store codecs: with them they import, without them the user is told
// which extension to install.
void testWindowsImageCodecs()
{
    std::printf ("windows image codecs\n");
    auto dir = makeTempDir ("wic");
    auto user = dir.getChildFile ("user");
    GalleryStore store (user, dir.getChildFile ("bundled"));
    struct Optional { const char* file; const unsigned char* bytes; size_t size; const char* ext; const char* store; };
    const Optional optional[] = {
        { "web.webp", ImageFixtures::webp, sizeof (ImageFixtures::webp), "webp", "Webp Image Extensions" },
        { "phone.heic", ImageFixtures::heic, sizeof (ImageFixtures::heic), "heic", "HEVC Video Extensions" },
    };
    for (const auto& f : optional)
    {
        auto file = dir.getChildFile (f.file);
        writeBytes (file, f.bytes, f.size);
        juce::String error;
        const auto decodable = WicImage::decodeGrey (file, 32).isValid();
        const auto imported = store.importImages ({ file }, error);
        std::printf ("  %s: codec %s, decodable %d\n", f.ext, WicImage::hasCodec (f.ext) ? "present" : "missing", (int) decodable);
        if (decodable)
            check (imported == 1 && user.getChildFile (juce::String (f.file).upToLastOccurrenceOf (".", false, false) + "." + f.ext).existsAsFile(),
                   juce::String (f.ext) + " decodable: imports" + (error.isEmpty() ? juce::String() : " (" + error + ")"));
        else
            check (imported == 0 && error.contains (f.store), juce::String (f.ext) + " not decodable: rejected, names the Store extension (" + error + ")");
    }
    dir.deleteRecursively();
}

// WIC thumbnail decode used by the CRT gallery: longest side capped, luminance times alpha.
void testWicThumbnail()
{
    std::printf ("wic thumbnail\n");
    auto dir = makeTempDir ("wicthumb");
    juce::Image image (juce::Image::ARGB, 1200, 600, true);   // left half transparent, right half white
    image.clear ({ 600, 0, 600, 600 }, juce::Colours::white);
    auto png = dir.getChildFile ("half.png");
    {
        juce::FileOutputStream out (png);
        juce::PNGImageFormat().writeImageToStream (image, out);
    }
    const auto grey = WicImage::decodeGrey (png, 512);
    check (grey.width == 512 && grey.height == 256, "1200x600 decodes to 512x256 (" + juce::String (grey.width) + "x" + juce::String (grey.height) + ")");
    if (grey.isValid())
        check (grey.pixels[(size_t) (128 * grey.width + 64)] == 0 && grey.pixels[(size_t) (128 * grey.width + 448)] == 255,
               "transparent -> 0, white -> 255");
    const auto small = WicImage::decodeGrey (png, 4096);
    check (small.width == 1200 && small.height == 600, "never enlarged");
    auto text = dir.getChildFile ("text.png");
    text.replaceWithText ("not an image");
    check (! WicImage::decodeGrey (text, 512).isValid() && ! WicImage::probe (text).isDecodable, "text file is not decodable");
    for (const auto& [name, bytes, size] : { std::tuple ("png", ImageFixtures::png, sizeof (ImageFixtures::png)),
                                              std::tuple ("gif", ImageFixtures::gif, sizeof (ImageFixtures::gif)),
                                              std::tuple ("jpg", ImageFixtures::jpg, sizeof (ImageFixtures::jpg)) })
    {
        juce::MemoryInputStream in (bytes, size, false);
        check (ImageHeader::pixelCount (in) == 64, juce::String (name) + " header: 8x8 before decoding");
    }
    {
        std::vector<juce::uint8> bomb (ImageFixtures::png, ImageFixtures::png + sizeof (ImageFixtures::png));
        bomb[16] = 0; bomb[17] = 0; bomb[18] = 0x4e; bomb[19] = 0x20;   // width 20000
        bomb[20] = 0; bomb[21] = 0; bomb[22] = 0x4e; bomb[23] = 0x20;   // height 20000
        juce::MemoryInputStream in (bomb.data(), bomb.size(), false);
        check (ImageHeader::pixelCount (in) == 400'000'000, "PNG claiming 20000x20000 is seen from its header");
        juce::MemoryInputStream text ("not an image", 12, false);
        check (ImageHeader::pixelCount (text) == -1, "unknown header -> -1");
    }
    const auto tiff = dir.getChildFile ("scan.tif");
    writeBytes (tiff, ImageFixtures::tiff, sizeof (ImageFixtures::tiff));
    const auto info = WicImage::probe (tiff);
    check (info.isDecodable && info.extension == "tiff" && info.width == 8 && info.height == 8, "TIFF probed as 8x8 tiff");
    dir.deleteRecursively();
}

// HKCU Run entry round trip. Restores whatever was registered before (a developer's own install).
void testLoginItem()
{
    std::printf ("login item\n");
    const juce::String runValue ("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\OscillaFX");
    const auto hadValue = juce::WindowsRegistry::valueExists (runValue);
    const auto previous = juce::WindowsRegistry::getValue (runValue);

    check (LoginItem::setEnabled (true) && LoginItem::isEnabled(), "register: enabled");
    const auto command = juce::WindowsRegistry::getValue (runValue);
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    check (command == exe.quoted() + " --launched-at-login", "Run value is \"<exe>\" --launched-at-login (" + command + ")");
    juce::WindowsRegistry::setValue (runValue, juce::String ("\"C:\\Elsewhere\\OscillaFX.exe\" --launched-at-login"));
    check (! LoginItem::isEnabled(), "entry of another copy counts as off");
    check (LoginItem::setEnabled (false) && ! LoginItem::isEnabled() && ! juce::WindowsRegistry::valueExists (runValue), "unregister removes the value");
    check (LoginItem::setEnabled (false), "unregister twice is fine");

    if (hadValue)
        juce::WindowsRegistry::setValue (runValue, previous);
}
#endif

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    testDeviceListing();
    testProfileStore();
    testRingBufferThreaded();
    testDriftCompensation();
    testTripleBuffer();
    testDspMapping();
    testRegistryIsolation();
    testPresets();
    testRestoreRecord();
    testDeviceKinds();
    testStarterProfiles();
    testSuggestionFlag();
    testSuggestionTracker();
    testGalleryStore();
    testAppPrefs();
    testSafetySettings();
    testDynamics();
   #if JUCE_WINDOWS
    testWasapiFormat();
    testStereoMaps();
    testEndpointTraits();
    testDefaultRoles();
   #endif
    testControllerApi();
   #if JUCE_WINDOWS
    testWindowsImageCodecs();
    testWicThumbnail();
    testLoginItem();
   #endif
    std::printf (failures ? "FAILED: %d\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
