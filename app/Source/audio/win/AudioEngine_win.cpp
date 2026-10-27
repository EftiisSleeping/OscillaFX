#include "audio/AudioEngine.h"

#include "audio/win/DefaultRoles.h"
#include "audio/win/MMDeviceHelpers.h"
#include "audio/win/StreamThreads.h"
#include "audio/win/WasapiFormat.h"
#include "platform/CrashWatchdog.h"
#include "platform/win/CrashGuardWin.h"
#include "audio/DriftResampler.h"
#include "audio/Dynamics.h"
#include "audio/DspParams.h"
#include "audio/SpscRingBuffer.h"
#include "audio/TripleBuffer.h"
#include "audio/VisualTaps.h"
#include "DfxDsp.h"

#include <juce_events/juce_events.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <thread>
#include <vector>

#pragma comment (lib, "avrt.lib")
#pragma comment (lib, "ole32.lib")

namespace MM = MMDeviceHelpers;
using MM::ComPtr;

// Same design as FxSound's audiopassthru: FxSound's virtual endpoint is the system default, its
// output is captured in WASAPI shared loopback, processed and rendered to the real endpoint.
//
// Threads: the message thread owns every COM object and opens / closes the streams. While routing,
// the capture thread is the only user of the capture client and the DfxDsp (the DSP registry is
// thread_local, see engine/platform/dsp_platform.cpp: after each (re)start the first capture wake-up
// re-applies the full parameter set), the render thread the only user of the render client. Both
// run under MMCSS "Pro Audio" and never allocate, lock or log; faults are reported through atomics
// that the message thread polls.

namespace
{
constexpr int MAX_BLOCK_FRAMES = 4096;
constexpr size_t RING_FRAMES = 1 << 16;
constexpr size_t MIN_TARGET_FILL = 512;
constexpr int VIRTUAL_CHANNELS = 2;                     // DSP is stereo; Windows downmixes for us
constexpr REFERENCE_TIME CAPTURE_BUFFER_HNS = 2000000;  // 200 ms: slack if the capture thread is late
constexpr REFERENCE_TIME RENDER_BUFFER_HNS = 300000;    // 30 ms
constexpr DWORD CAPTURE_WAIT_MS = 10;                   // loopback events do not fire on every Windows build
constexpr DWORD RENDER_WAIT_MS = 100;
constexpr DWORD PAUSED_POLL_MS = 10;
constexpr double IDLE_PAUSE_SECONDS = 2.0;              // stop the real stream when silent, so the PC can sleep
constexpr int DEVICE_CHECK_DELAY_MS = 100;              // coalesces bursts of endpoint notifications
constexpr int CAPTURE_START_TIMEOUT_MS = 2000;          // capture thread must have polled the stream by then
constexpr int HEALTH_POLL_MS = 200;
constexpr juce::uint32 FAULT_WINDOW_MS = 2000;
constexpr int MAX_FAULT_RESTARTS = 3;

constexpr DWORD CAPTURE_FLAGS = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE;
constexpr DWORD RENDER_FLAGS = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE;

const char* const VIRTUAL_MISSING = "Virtual device \"FxSound Audio Enhancer\" not found. Reinstall OscillaFX.";
const char* const FXSOUND_RUNNING_UTF8 =
    "FxSound is running. Quit FxSound to use OscillaFX \xe2\x80\x94 they share the same virtual device.";
const char* const CAPTURE_BLOCKED =
    "Windows blocked audio capture from \"FxSound Audio Enhancer\". Turn on \"Let desktop apps access your "
    "microphone\" in Settings > Privacy > Microphone, then turn OscillaFX on again.";

// FxSound's own app drives the same virtual endpoint (default device, volume, its own passthrough).
bool isFxSoundAppRunning()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof (entry);
    bool found = false;
    for (BOOL more = Process32FirstW (snapshot, &entry); more && ! found; more = Process32NextW (snapshot, &entry))
        found = _wcsicmp (entry.szExeFile, L"FxSound.exe") == 0;
    CloseHandle (snapshot);
    return found;
}

bool isAccessDenied (HRESULT hr) { return hr == E_ACCESSDENIED; }

juce::String captureErrorText (HRESULT hr)
{
    if (isAccessDenied (hr))
        return CAPTURE_BLOCKED;
    return "Could not capture \"FxSound Audio Enhancer\" (error 0x" + juce::String::toHexString ((int) hr) + ").";
}

// FxSound runs its endpoint at 48 kHz for the 48 kHz family of output rates, else 44.1 kHz.
int virtualRateFor (double outputRate)
{
    return ((int) outputRate % 48000) == 0 ? 48000 : 44100;
}

// MMCSS registration for the calling thread.
class ProAudioPriority
{
public:
    ProAudioPriority() : task (AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex)) {}
    ~ProAudioPriority() { if (task != nullptr) AvRevertMmThreadCharacteristics (task); }
    ProAudioPriority (const ProAudioPriority&) = delete;
    ProAudioPriority& operator= (const ProAudioPriority&) = delete;

private:
    DWORD taskIndex = 0;
    HANDLE task;
};

ComPtr<IAudioClient> activateClient (IMMDevice& device)
{
    ComPtr<IAudioClient> client;
    device.Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) client.GetAddressOf());
    return client;
}

struct SystemEndpoints : DefaultRoles::Endpoints
{
    explicit SystemEndpoints (IMMDeviceEnumerator& e) : enumerator (e) {}

    juce::String defaultFor (DefaultRoles::Role role) const override { return MM::defaultRenderId (enumerator, (ERole) role); }
    bool setDefault (const juce::String& id, DefaultRoles::Role role) override { return MM::setDefaultEndpoint (id, (ERole) role); }
    bool isActive (const juce::String& id) const override { return MM::isActiveRender (enumerator, id); }

    IMMDeviceEnumerator& enumerator;
};

// The real device's own volume and mute, put back when OscillaFX stops driving it (as FxSound).
struct SavedVolume
{
    juce::String id;
    float level = 0.0f;
    BOOL muted = FALSE;
    bool valid = false;
};

enum class RouteResult { ok, outputFailed, captureFailed };

// engine_selftest --live-audio on a CI runner whose only endpoint is FxSound's: route the virtual
// endpoint onto itself so start / stop run for real. Compiled into the self-test only.
bool virtualOutputAllowedForTest()
{
   #if OSCILLA_ENGINE_SELFTEST
    return juce::SystemStats::getEnvironmentVariable ("OSCILLA_TEST_VIRTUAL_OUTPUT", {}) == "1";
   #else
    return false;
   #endif
}
} // namespace

struct AudioEngine::Impl
{
    explicit Impl (AudioEngine& o) : owner (o)
    {
        CoCreateGuid (&volumeContext);
        if (enumerator != nullptr)
            virtualId = MM::findFxSoundEndpoint (*enumerator.Get()); // read-only lookup so device lists exclude it before start()
        DspParamsApply::configureBands (dsp);
        registry()[token] = this;
    }

    ~Impl()
    {
        stop();
        registry().erase (token);
    }

    // ---------------------------------------------------------------- devices

    bool isUsableOutput (const juce::String& id) const
    {
        if (enumerator == nullptr || id.isEmpty() || ! MM::isActiveRender (*enumerator.Get(), id))
            return false;
        if (virtualOutputAllowed)
            return true;
        auto device = MM::deviceById (*enumerator.Get(), id);
        return id != virtualId && device != nullptr && ! MM::isFxSoundEndpoint (MM::describe (*device.Get()));
    }

    juce::Array<MM::Endpoint> usableEndpoints() const
    {
        juce::Array<MM::Endpoint> result;
        if (enumerator == nullptr)
            return result;
        for (auto& endpoint : MM::renderEndpoints (*enumerator.Get()))
            if (virtualOutputAllowed || (endpoint.id != virtualId && ! MM::isFxSoundEndpoint (endpoint)))
                result.add (endpoint);
        return result;
    }

    // The endpoint that was default before OscillaFX took over, then the first usable one.
    juce::String fallbackOutput() const
    {
        if (auto original = takeover.original (DefaultRoles::console); isUsableOutput (original))
            return original;
        auto endpoints = usableEndpoints();
        return endpoints.isEmpty() ? juce::String() : endpoints.getFirst().id;
    }

    // Explicit choice, then the current system default, then the endpoint saved before a crash.
    juce::String resolveInitialOutput() const
    {
        if (isUsableOutput (preferredId))
            return preferredId;
        if (enumerator != nullptr)
            if (auto current = MM::defaultRenderId (*enumerator.Get(), eConsole); isUsableOutput (current))
                return current;
        if (auto saved = CrashWatchdog::readRestoreUid(); isUsableOutput (saved))
            return saved;
        return fallbackOutput();
    }

    OutputDevice describeOutput (const juce::String& id) const
    {
        if (id.isEmpty() || enumerator == nullptr)
            return {};
        auto device = MM::deviceById (*enumerator.Get(), id);
        return { id, device != nullptr ? MM::describe (*device.Get()).name : juce::String() };
    }

    juce::Array<OutputDevice> outputDevices() const
    {
        juce::Array<OutputDevice> result;
        for (auto& endpoint : usableEndpoints())
            result.add ({ endpoint.id, endpoint.name });
        return result;
    }

    OutputDevice currentOutput() const
    {
        return describeOutput (routing ? outputId : resolveInitialOutput());
    }

    // ---------------------------------------------------------------- lifecycle

    bool start()
    {
        if (running)
            return true;
        error.clear();
        if (enumerator == nullptr)
            enumerator = MM::createEnumerator();
        virtualId = enumerator != nullptr ? MM::findFxSoundEndpoint (*enumerator.Get()) : juce::String();
        if (virtualId.isEmpty())
            return fail (VIRTUAL_MISSING);
        if (isFxSoundAppRunning())
            return fail (juce::String (juce::CharPointer_UTF8 (FXSOUND_RUNNING_UTF8)));

        auto initial = resolveInitialOutput();
        if (initial.isEmpty())
            return failAndRestore ("No output device available.");

        running = true;
        faultRestarts = 0;
        captureHeartbeat.store (0, std::memory_order_relaxed);
        if (! routeWithFallback (initial))
        {
            running = false;
            return failAndRestore (error);
        }
        SystemEndpoints endpoints (*enumerator.Get());
        if (! takeover.begin (endpoints, virtualId))
        {
            stop();
            return fail ("Could not make \"FxSound Audio Enhancer\" the default playback device.");
        }
        registerDeviceNotifications();
        captureWatch.startTimer (CAPTURE_START_TIMEOUT_MS);
        healthCheck.startTimer (HEALTH_POLL_MS);
        return true;
    }

    void stop()
    {
        if (! running)
            return;
        running = false;
        deviceCheck.stopTimer();
        captureWatch.stopTimer();
        healthCheck.stopTimer();
        unregisterDeviceNotifications();
        stopRouting();
        restoreSavedVolume (true);
        savedVolume = {};   // handed back: the next start saves the device's level afresh
        restoreSystemDefault();
    }

    bool fail (const juce::String& message)
    {
        error = message;
        return false;
    }

    bool failAndRestore (const juce::String& message)
    {
        if (enumerator != nullptr && MM::defaultRenderId (*enumerator.Get(), eConsole) == virtualId)
            restoreSystemDefault();
        return fail (message);
    }

    void restoreSystemDefault()
    {
        auto target = isUsableOutput (outputId) ? outputId : resolveInitialOutput();
        if (enumerator != nullptr)
        {
            SystemEndpoints endpoints (*enumerator.Get());
            if (takeover.isActive())
                takeover.restore (endpoints, target);
            else if (target.isNotEmpty() && endpoints.defaultFor (DefaultRoles::console) == virtualId)
                for (auto role : { DefaultRoles::console, DefaultRoles::multimedia })
                    endpoints.setDefault (target, role);
        }
        CrashGuard::setRestoreEndpoint (nullptr);
        CrashWatchdog::clearRestoreUid();
    }

    // In-process (CrashGuard) and out-of-process (watchdog) crash restore both target the real output.
    void rememberOutput()
    {
        preferredId = outputId;
        CrashGuard::setRestoreEndpoint (outputId.toWideCharPointer());
        if (! CrashWatchdog::writeRestoreUid (outputId))
            juce::Logger::writeToLog ("OscillaFX: could not write " + CrashWatchdog::restoreFile().getFullPathName());
    }

    // ---------------------------------------------------------------- routing

    RouteResult routeTo (const juce::String& id)
    {
        stopRouting();
        outputId = id;
        auto result = startRouting();
        if (result != RouteResult::ok)
            return result;
        rememberOutput();
        attachVolumes();
        return result;
    }

    // Tries `first`, then every other output. Sets `error` describing the outcome.
    bool routeWithFallback (const juce::String& first)
    {
        juce::StringArray candidates;
        candidates.add (first);
        for (auto& endpoint : usableEndpoints())
            candidates.addIfNotAlreadyThere (endpoint.id);
        for (auto& id : candidates)
        {
            const auto result = routeTo (id);
            if (result == RouteResult::captureFailed)
                return false; // `error` set; another output would not help
            if (result != RouteResult::ok)
                continue;
            error = id == first ? juce::String()
                                : "Could not start audio on " + describeOutput (first).name + "; using " + describeOutput (id).name + ".";
            return true;
        }
        error = "No output device could be started.";
        return false;
    }

    void switchOutput (const juce::String& newId, bool force = false)
    {
        if (newId.isEmpty() || (! force && newId == outputId && routing))
            return;
        if (! routeWithFallback (newId))
        {
            auto message = error;
            stop();
            error = message;
        }
        notifyOutputChanged();
    }

    RouteResult startRouting()
    {
        auto output = MM::deviceById (*enumerator.Get(), outputId);
        auto virtualDevice = MM::deviceById (*enumerator.Get(), virtualId);
        if (output == nullptr)
            return RouteResult::outputFailed;
        // Before any stream is open: a format change invalidates streams on that endpoint.
        MM::matchDeviceFormat (virtualId, virtualRateFor (mixRate (*output.Get())), VIRTUAL_CHANNELS);
        if (! openRender (*output.Get()))
        {
            stopRouting();
            return RouteResult::outputFailed;
        }
        const HRESULT hr = virtualDevice != nullptr ? openCapture (*virtualDevice.Get()) : AUDCLNT_E_DEVICE_INVALIDATED;
        if (FAILED (hr))
        {
            stopRouting();
            error = captureErrorText (hr);
            return RouteResult::captureFailed;
        }

        prepareDsp();
        renderMap = WasapiFormat::stereoMapFor (renderFormat.channels, renderFormat.channelMask);
        captureMix = WasapiFormat::downmixFor (captureFormat.channels, captureFormat.channelMask);
        ring.reset();
        resampler.prepare (captureFormat.sampleRate, renderFormat.sampleRate, targetFill());
        prefillSilence();

        captureFault.store (S_OK);
        renderFault.store (S_OK);
        streams.start ([this] (HANDLE stop) { captureLoop (stop); }, [this] (HANDLE stop) { renderLoop (stop); });
        if (FAILED (captureClient->Start()) || FAILED (renderClient->Start()))
        {
            stopRouting();
            return RouteResult::outputFailed;
        }
        routing = true;
        return RouteResult::ok;
    }

    // Two capture periods plus the render buffer, as the macOS engine's 2 x (in + out) buffers.
    size_t targetFill() const
    {
        REFERENCE_TIME period = 0;
        captureClient->GetDevicePeriod (&period, nullptr);
        const auto captureFrames = (size_t) ((double) period * captureFormat.sampleRate / 1.0e7);
        const auto renderFrames = (size_t) ((double) renderBufferFrames * captureFormat.sampleRate / renderFormat.sampleRate);
        return std::max (2 * captureFrames + renderFrames, MIN_TARGET_FILL);
    }

    static double mixRate (IMMDevice& device)
    {
        auto client = activateClient (device);
        WAVEFORMATEX* mix = nullptr;
        if (client == nullptr || FAILED (client->GetMixFormat (&mix)) || mix == nullptr)
            return 0;
        const double rate = mix->nSamplesPerSec;
        CoTaskMemFree (mix);
        return rate;
    }

    // Shared mode in the endpoint's mix format, or the closest one it accepts (some Bluetooth
    // drivers reject their own mix format, as FxSound found).
    bool openRender (IMMDevice& device)
    {
        auto client = activateClient (device);
        WAVEFORMATEX* mix = nullptr;
        if (client == nullptr || FAILED (client->GetMixFormat (&mix)) || mix == nullptr)
            return false;
        WAVEFORMATEX* closest = nullptr;
        const bool useClosest = client->IsFormatSupported (AUDCLNT_SHAREMODE_SHARED, mix, &closest) == S_FALSE && closest != nullptr;
        const WAVEFORMATEX* chosen = useClosest ? closest : mix;
        HRESULT hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, RENDER_FLAGS, RENDER_BUFFER_HNS, 0, chosen, nullptr);
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT && closest != nullptr && ! useClosest)
        {
            chosen = closest;
            client = activateClient (device);
            hr = client != nullptr ? client->Initialize (AUDCLNT_SHAREMODE_SHARED, RENDER_FLAGS, RENDER_BUFFER_HNS, 0, chosen, nullptr) : E_FAIL;
        }
        if (SUCCEEDED (hr))
            renderFormat = MM::streamFormat (*chosen);
        CoTaskMemFree (mix);
        CoTaskMemFree (closest);

        if (FAILED (hr) || ! renderFormat.isUsable() || renderFormat.bytesPerFrame() <= 0
            || FAILED (client->GetBufferSize (&renderBufferFrames))
            || FAILED (client->GetService (IID_PPV_ARGS (&renderService)))
            || FAILED (client->SetEventHandle (renderEvent.get())))
            return false;
        renderClient = client;
        return true;
    }

    // Loopback of the virtual render endpoint. Event-driven where Windows supports it for loopback,
    // otherwise the capture thread's short wait timeout polls.
    HRESULT openCapture (IMMDevice& device)
    {
        auto client = activateClient (device);
        WAVEFORMATEX* mix = nullptr;
        HRESULT hr = client != nullptr ? client->GetMixFormat (&mix) : E_FAIL;
        if (FAILED (hr) || mix == nullptr)
            return FAILED (hr) ? hr : E_FAIL;

        bool eventDriven = true;
        hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, CAPTURE_FLAGS | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 CAPTURE_BUFFER_HNS, 0, mix, nullptr);
        if (FAILED (hr) && ! isAccessDenied (hr))
        {
            eventDriven = false;
            client = activateClient (device);
            hr = client != nullptr ? client->Initialize (AUDCLNT_SHAREMODE_SHARED, CAPTURE_FLAGS, CAPTURE_BUFFER_HNS, 0, mix, nullptr) : E_FAIL;
        }
        captureFormat = MM::streamFormat (*mix);
        CoTaskMemFree (mix);
        if (FAILED (hr))
            return hr;
        if (! captureFormat.isUsable())
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (eventDriven && FAILED (hr = client->SetEventHandle (captureEvent.get())))
            return hr;
        if (FAILED (hr = client->GetService (IID_PPV_ARGS (&captureService))))
            return hr;
        captureClient = client;
        return S_OK;
    }

    void prefillSilence()
    {
        BYTE* data = nullptr;
        if (SUCCEEDED (renderService->GetBuffer (renderBufferFrames, &data)))
            renderService->ReleaseBuffer (renderBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    // Message thread, threads stopped. Registry-touching parameter writes are left to the capture thread.
    void prepareDsp()
    {
        dsp.setSignalFormat (32, SpscRingBuffer::NUM_CHANNELS, (int) captureFormat.sampleRate, 32);
        dynamics.prepare (captureFormat.sampleRate);
        needsFullApply.store (true, std::memory_order_release);
    }

    void stopRouting()
    {
        streams.stop();
        if (renderClient != nullptr)
            renderClient->Stop();
        if (captureClient != nullptr)
            captureClient->Stop();
        renderService.Reset();
        captureService.Reset();
        renderClient.Reset();
        captureClient.Reset();
        releaseVolumes();
        taps.clear();
        routing = false;
    }

    void setOutputDevice (const juce::String& uid)
    {
        if (! isUsableOutput (uid))
            return;
        preferredId = uid;
        if (running)
            switchOutput (uid);
        else
            notifyOutputChanged();
    }

    // Delivered later on the message thread; reports the device current at delivery time.
    void notifyOutputChanged()
    {
        post (token, [] (Impl& live)
        {
            if (live.owner.onOutputDeviceChanged)
                live.owner.onOutputDeviceChanged (live.currentOutput());
        });
    }

    // ---------------------------------------------------------------- volume mirroring
    // The virtual endpoint carries the system volume (keyboard keys, tray slider) and the real
    // endpoint follows it, both ways. On attach the real endpoint's level is copied onto the virtual
    // one (as the macOS engine does), so the speakers keep the level they had. The real endpoint's
    // own setting is saved and put back when OscillaFX leaves it: in full when switching to another
    // output, never louder than now when stopping (as FxSound).
    // No gain compensation on the captured signal, as FxSound: WASAPI loopback taps the endpoint's
    // mix before its endpoint volume is applied, so the level is applied once, on the real endpoint.
    // (macOS compensates because the HAL driver scales its own stream by its volume.)

    ComPtr<IAudioEndpointVolume> activateVolume (const juce::String& id) const
    {
        ComPtr<IAudioEndpointVolume> volume;
        if (auto device = MM::deviceById (*enumerator.Get(), id))
            device->Activate (__uuidof (IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**) volume.GetAddressOf());
        return volume;
    }

    void copyVolume (IAudioEndpointVolume& from, IAudioEndpointVolume& to)
    {
        float level = 0.0f;
        BOOL muted = FALSE;
        if (SUCCEEDED (from.GetMasterVolumeLevelScalar (&level)))
            to.SetMasterVolumeLevelScalar (level, &volumeContext);
        if (SUCCEEDED (from.GetMute (&muted)))
            to.SetMute (muted, &volumeContext);
    }

    void saveOutputVolume()
    {
        SavedVolume next { outputId };
        next.valid = SUCCEEDED (outputVolume->GetMasterVolumeLevelScalar (&next.level))
                  && SUCCEEDED (outputVolume->GetMute (&next.muted));
        if (next.valid)
            savedVolume = next;
    }

    void attachVolumes()
    {
        virtualVolume = activateVolume (virtualId);
        outputVolume = activateVolume (outputId);
        if (virtualVolume == nullptr || outputVolume == nullptr)
            return releaseVolumes();
        if (savedVolume.valid && savedVolume.id != outputId)
        {
            restoreSavedVolume (false);
            savedVolume = {};
        }
        if (! savedVolume.valid)
            saveOutputVolume();
        copyVolume (*outputVolume.Get(), *virtualVolume.Get());

        virtualVolumeEvents.Attach (new VolumeNotifications (token, true, volumeContext));
        outputVolumeEvents.Attach (new VolumeNotifications (token, false, volumeContext));
        virtualVolume->RegisterControlChangeNotify (virtualVolumeEvents.Get());
        outputVolume->RegisterControlChangeNotify (outputVolumeEvents.Get());
    }

    void releaseVolumes()
    {
        if (virtualVolume != nullptr && virtualVolumeEvents != nullptr)
            virtualVolume->UnregisterControlChangeNotify (virtualVolumeEvents.Get());
        if (outputVolume != nullptr && outputVolumeEvents != nullptr)
            outputVolume->UnregisterControlChangeNotify (outputVolumeEvents.Get());
        virtualVolumeEvents.Reset();
        outputVolumeEvents.Reset();
        virtualVolume.Reset();
        outputVolume.Reset();
    }

    void restoreSavedVolume (bool neverRaise)
    {
        if (! savedVolume.valid || enumerator == nullptr)
            return;
        auto volume = activateVolume (savedVolume.id);
        if (volume == nullptr)
            return;
        float level = savedVolume.level, current = 0.0f;
        if (neverRaise && SUCCEEDED (volume->GetMasterVolumeLevelScalar (&current)))
            level = std::min (level, current);
        volume->SetMasterVolumeLevelScalar (level, &volumeContext);
        volume->SetMute (savedVolume.muted, &volumeContext);
    }

    void onVolumeChanged (bool fromVirtual)
    {
        if (! routing || virtualVolume == nullptr || outputVolume == nullptr)
            return;
        if (fromVirtual)
            return copyVolume (*virtualVolume.Get(), *outputVolume.Get());
        copyVolume (*outputVolume.Get(), *virtualVolume.Get());
        saveOutputVolume(); // the user set the real device itself: that is its own setting now
    }

    // ---------------------------------------------------------------- endpoint notifications

    void registerDeviceNotifications()
    {
        deviceEvents.Attach (new DeviceNotifications (token));
        if (FAILED (enumerator->RegisterEndpointNotificationCallback (deviceEvents.Get())))
            deviceEvents.Reset();
    }

    void unregisterDeviceNotifications()
    {
        if (deviceEvents != nullptr)
            enumerator->UnregisterEndpointNotificationCallback (deviceEvents.Get());
        deviceEvents.Reset();
    }

    // A real endpoint picked in Windows sound settings becomes our output; the virtual one stays default.
    void onDefaultChanged (ERole role, const juce::String& id)
    {
        if (! running || id.isEmpty() || id == virtualId)
            return;
        SystemEndpoints endpoints (*enumerator.Get());
        if (role == eCommunications)
            return takeover.userChose (endpoints, DefaultRoles::communications, virtualId);
        if (! isUsableOutput (id))
            return;
        preferredId = id;
        switchOutput (id);
        if (running)
            takeover.userChose (endpoints, (DefaultRoles::Role) role, virtualId);
    }

    void onDevicesChanged()
    {
        deviceCheck.stopTimer();
        if (! running)
            return;
        if (! MM::isActiveRender (*enumerator.Get(), virtualId))
            return stopWithError ("The virtual audio device disappeared.");
        if (isUsableOutput (outputId))
            return;
        auto fallback = fallbackOutput();
        if (fallback.isEmpty())
            return stopWithError ("No output device available.");
        switchOutput (fallback);
    }

    // A stream thread hit an error (device removed, format changed, exclusive-mode grab): reopen.
    void onHealthCheck()
    {
        if (! running || (captureFault.load() == S_OK && renderFault.load() == S_OK))
            return;
        const auto now = juce::Time::getMillisecondCounter();
        faultRestarts = now - lastFaultMs < FAULT_WINDOW_MS ? faultRestarts + 1 : 1;
        lastFaultMs = now;
        if (faultRestarts > MAX_FAULT_RESTARTS)
            return stopWithError ("The audio device stopped responding; system output restored.");
        if (! MM::isActiveRender (*enumerator.Get(), virtualId) || ! isUsableOutput (outputId))
            return onDevicesChanged();
        switchOutput (outputId, true);
    }

    // Safety net: the system output is already ours, so a capture thread that never polls means silence.
    void onCaptureWatch()
    {
        captureWatch.stopTimer();
        if (running && captureHeartbeat.load (std::memory_order_relaxed) == 0)
            stopWithError ("No audio from the virtual device; system output restored.");
    }

    void stopWithError (const juce::String& message)
    {
        stop();
        error = message;
        notifyOutputChanged();
    }

    // ---------------------------------------------------------------- audio threads

    void captureLoop (HANDLE stop)
    {
        MM::ComScope threadCom (COINIT_MULTITHREADED);
        ProAudioPriority priority;
        while (! StreamThreads::shouldExit (stop, captureEvent.get(), CAPTURE_WAIT_MS))
        {
            const HRESULT hr = drainCapture();
            if (FAILED (hr))
                return captureFault.store (hr);
            captureHeartbeat.fetch_add (1, std::memory_order_relaxed);
        }
    }

    HRESULT drainCapture()
    {
        applyPendingParams();
        bool processed = false;
        UINT32 packet = 0;
        HRESULT hr = captureService->GetNextPacketSize (&packet);
        while (SUCCEEDED (hr) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED (hr = captureService->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                return hr;
            processCapture (data, (int) frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            processed |= frames > 0;
            if (FAILED (hr = captureService->ReleaseBuffer (frames)))
                return hr;
            hr = captureService->GetNextPacketSize (&packet);
        }
        if (processed)
            publishSpectrum();
        return hr;
    }

    void applyPendingParams()
    {
        if (needsFullApply.exchange (false, std::memory_order_acq_rel))
        {
            pendingParams.fetch (appliedParams);
            DspParamsApply::applyAll (dsp, appliedParams);
            dynamics.configure (appliedParams.nightMode, appliedParams.limiterCeilingDb);
            return;
        }
        DspParams latest;
        if (! pendingParams.fetch (latest))
            return;
        DspParamsApply::applyChanges (dsp, latest, appliedParams);
        appliedParams = latest;
        dynamics.configure (appliedParams.nightMode, appliedParams.limiterCeilingDb);
    }

    void processCapture (const BYTE* data, int total, bool silent)
    {
        const int frameBytes = captureFormat.bytesPerFrame();
        for (int offset = 0; offset < total; offset += MAX_BLOCK_FRAMES)
        {
            const int frames = std::min (MAX_BLOCK_FRAMES, total - offset);
            if (silent || data == nullptr)
                std::fill (captureScratch.begin(), captureScratch.begin() + 2 * frames, 0.0f);
            else
                WasapiFormat::toStereo (data + (size_t) offset * (size_t) frameBytes, captureFormat, captureMix, captureScratch.data(), frames);
            dsp.processAudio ((short*) captureScratch.data(), (short*) processedScratch.data(), frames, 0);
            dynamics.process (processedScratch.data(), frames); // night mode, then the safety limiter: last in the chain
            taps.writeScope (processedScratch.data(), frames);
            ring.write (processedScratch.data(), (size_t) frames);
        }
    }

    void publishSpectrum()
    {
        float bands[VisualTaps::NUM_SPECTRUM_BANDS];
        dsp.getSpectrumBandValues (bands, VisualTaps::NUM_SPECTRUM_BANDS);
        taps.writeSpectrum (bands);
    }

    void renderLoop (HANDLE stop)
    {
        MM::ComScope threadCom (COINIT_MULTITHREADED);
        ProAudioPriority priority;
        const auto idleLimit = (UINT64) (IDLE_PAUSE_SECONDS * renderFormat.sampleRate);
        UINT64 idleFrames = 0;
        bool paused = false;
        while (! StreamThreads::shouldExit (stop, renderEvent.get(), paused ? PAUSED_POLL_MS : RENDER_WAIT_MS))
        {
            HRESULT hr = S_OK;
            if (paused)
            {
                if (ring.availableToRead() < resampler.targetFill())
                    continue;
                hr = renderClient->Start();
                paused = false;
                idleFrames = 0;
            }
            else
            {
                bool audible = false;
                UINT32 written = 0;
                hr = fillRender (audible, written);
                idleFrames = audible ? 0 : idleFrames + written;
                if (SUCCEEDED (hr) && idleFrames > idleLimit)
                {
                    hr = renderClient->Stop();
                    paused = true;
                }
            }
            if (FAILED (hr))
                return renderFault.store (hr);
        }
    }

    // Tops the endpoint buffer up from the ring. `audible` false while the resampler is priming.
    HRESULT fillRender (bool& audible, UINT32& written)
    {
        UINT32 padding = 0;
        HRESULT hr = renderClient->GetCurrentPadding (&padding);
        if (FAILED (hr) || padding >= renderBufferFrames)
            return hr;
        const int total = (int) (renderBufferFrames - padding);
        BYTE* data = nullptr;
        if (FAILED (hr = renderService->GetBuffer ((UINT32) total, &data)))
            return hr;
        const int frameBytes = renderFormat.bytesPerFrame();
        for (int offset = 0; offset < total; offset += MAX_BLOCK_FRAMES)
        {
            const int frames = std::min (MAX_BLOCK_FRAMES, total - offset);
            audible |= resampler.render (ring, renderScratch.data(), frames);
            WasapiFormat::fromStereo (renderScratch.data(), renderFormat, renderMap, data + (size_t) offset * (size_t) frameBytes, frames);
        }
        written = (UINT32) total;
        return renderService->ReleaseBuffer ((UINT32) total, 0);
    }

    // ---------------------------------------------------------------- notification objects
    // Called on system threads; they only forward to the message thread.

    using Task = std::function<void (Impl&)>;

    static void post (uintptr_t t, Task task)
    {
        juce::MessageManager::callAsync ([t, task = std::move (task)]
        {
            if (auto* live = lookup (t))
                task (*live);
        });
    }

    struct DeviceNotifications : MM::ComObject<IMMNotificationClient>
    {
        explicit DeviceNotifications (uintptr_t t) : token (t) {}

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged (EDataFlow flow, ERole role, LPCWSTR id) override
        {
            if (flow == eRender)
                post (token, [role, endpoint = juce::String (id != nullptr ? id : L"")] (Impl& live)
                      { live.onDefaultChanged (role, endpoint); });
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded (LPCWSTR) override { return scheduleDeviceCheck(); }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved (LPCWSTR) override { return scheduleDeviceCheck(); }
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged (LPCWSTR, DWORD) override { return scheduleDeviceCheck(); }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged (LPCWSTR, const PROPERTYKEY) override { return S_OK; }

        HRESULT scheduleDeviceCheck()
        {
            post (token, [] (Impl& live) { live.deviceCheck.startTimer (DEVICE_CHECK_DELAY_MS); });
            return S_OK;
        }

        const uintptr_t token;
    };

    struct VolumeNotifications : MM::ComObject<IAudioEndpointVolumeCallback>
    {
        VolumeNotifications (uintptr_t t, bool virtualSide, const GUID& ownContext)
            : token (t), fromVirtual (virtualSide), context (ownContext) {}

        HRESULT STDMETHODCALLTYPE OnNotify (PAUDIO_VOLUME_NOTIFICATION_DATA data) override
        {
            if (data != nullptr && ! IsEqualGUID (data->guidEventContext, context))
                post (token, [side = fromVirtual] (Impl& live) { live.onVolumeChanged (side); });
            return S_OK;
        }

        const uintptr_t token;
        const bool fromVirtual;
        const GUID context;
    };

    // ---------------------------------------------------------------- listener dispatch

    static std::map<uintptr_t, Impl*>& registry()
    {
        static std::map<uintptr_t, Impl*> live;
        return live;
    }

    static Impl* lookup (uintptr_t t)
    {
        auto it = registry().find (t);
        return it == registry().end() ? nullptr : it->second;
    }

    static uintptr_t nextToken()
    {
        static uintptr_t counter = 0;
        return ++counter;
    }

    // ---------------------------------------------------------------- state

    AudioEngine& owner;
    const uintptr_t token = nextToken();

    // Message thread
    MM::ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator = MM::createEnumerator();
    juce::String virtualId, preferredId, outputId, error;
    const bool virtualOutputAllowed = virtualOutputAllowedForTest();
    bool running = false, routing = false;
    DefaultRoles::Takeover takeover;
    GUID volumeContext {};
    SavedVolume savedVolume;
    ComPtr<IAudioEndpointVolume> virtualVolume, outputVolume;
    ComPtr<VolumeNotifications> virtualVolumeEvents, outputVolumeEvents;
    ComPtr<DeviceNotifications> deviceEvents;
    juce::uint32 lastFaultMs = 0;
    int faultRestarts = 0;
    juce::TimedCallback deviceCheck { [this] { onDevicesChanged(); } };
    juce::TimedCallback captureWatch { [this] { onCaptureWatch(); } };
    juce::TimedCallback healthCheck { [this] { onHealthCheck(); } };

    // Opened by the message thread while the stream threads are stopped
    ComPtr<IAudioClient> captureClient, renderClient;
    ComPtr<IAudioCaptureClient> captureService;
    ComPtr<IAudioRenderClient> renderService;
    WasapiFormat::StreamFormat captureFormat, renderFormat;
    WasapiFormat::Downmix captureMix;
    WasapiFormat::StereoMap renderMap;
    UINT32 renderBufferFrames = 0;
    MM::Event captureEvent, renderEvent;   // auto-reset: one wake-up per WASAPI buffer
    StreamThreads streams;

    // Shared with the stream threads (lock-free)
    TripleBuffer<DspParams> pendingParams;
    SpscRingBuffer ring { RING_FRAMES };
    VisualTaps taps;
    std::atomic<bool> needsFullApply { true };
    std::atomic<uint32_t> captureHeartbeat { 0 };
    std::atomic<HRESULT> captureFault { S_OK }, renderFault { S_OK };

    // Capture thread only (setSignalFormat also from the message thread while stopped)
    DfxDsp dsp;
    DspParams appliedParams = DspParams::fromSettings ({});
    Dynamics::DynamicsChain dynamics;
    std::vector<float> captureScratch = std::vector<float> (MAX_BLOCK_FRAMES * 2);
    std::vector<float> processedScratch = std::vector<float> (MAX_BLOCK_FRAMES * 2);

    // Render thread only
    DriftResampler resampler;
    std::vector<float> renderScratch = std::vector<float> (MAX_BLOCK_FRAMES * 2);
};

AudioEngine::AudioEngine() : impl (std::make_unique<Impl> (*this)) {}
AudioEngine::~AudioEngine() = default;

bool AudioEngine::start()                    { return impl->start(); }
void AudioEngine::stop()                     { impl->stop(); }
juce::String AudioEngine::lastError() const  { return impl->error; }

void AudioEngine::applySettings (const DspSettings& settings)
{
    impl->pendingParams.publish (DspParams::fromSettings (settings));
}

juce::Array<OutputDevice> AudioEngine::outputDevices() const { return impl->outputDevices(); }
OutputDevice AudioEngine::currentOutputDevice() const        { return impl->currentOutput(); }
void AudioEngine::setOutputDevice (const juce::String& uid)  { impl->setOutputDevice (uid); }

void AudioEngine::getSpectrum (float* bands10) const { impl->taps.readSpectrum (bands10); }

int AudioEngine::getScopeSamples (float* left, float* right, int maxSamples) const
{
    return impl->taps.readScope (left, right, maxSamples);
}
