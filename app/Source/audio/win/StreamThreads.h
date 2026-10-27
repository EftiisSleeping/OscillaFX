#pragma once

#include "audio/win/MMDeviceHelpers.h"

#include <array>
#include <functional>
#include <thread>

// The capture and render threads and the stop signal they share. The stop event is manual-reset:
// an auto-reset one would wake only one of the two waiters and stop() would never finish joining.
// start() / stop() on the message thread.
class StreamThreads
{
public:
    using Loop = std::function<void (HANDLE stopEvent)>;

    ~StreamThreads() { stop(); }

    void start (Loop first, Loop second)
    {
        stop();
        ResetEvent (stopEvent.get());
        threads[0] = std::thread ([this, loop = std::move (first)] { loop (stopEvent.get()); });
        threads[1] = std::thread ([this, loop = std::move (second)] { loop (stopEvent.get()); });
    }

    void stop()
    {
        SetEvent (stopEvent.get());
        for (auto& thread : threads)
            if (thread.joinable())
                thread.join();
    }

    // Stream thread side: waits for `work`, the stop signal or the timeout. True when the thread must exit.
    static bool shouldExit (HANDLE stop, HANDLE work, DWORD timeoutMs)
    {
        const HANDLE events[] = { stop, work };
        const DWORD result = WaitForMultipleObjects (2, events, FALSE, timeoutMs);
        return result == WAIT_OBJECT_0 || result == WAIT_FAILED;
    }

private:
    MMDeviceHelpers::Event stopEvent { true };
    std::array<std::thread, 2> threads;
};
