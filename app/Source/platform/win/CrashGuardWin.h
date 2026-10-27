#pragma once

#include "platform/CrashGuard.h"

namespace CrashGuard
{
// Real endpoint (IMMDevice::GetId) to make the default again if OscillaFX crashes or the session
// ends while routing; nullptr while not routing. Any thread. Copied into a preallocated buffer.
void setRestoreEndpoint (const wchar_t* endpointId);
} // namespace CrashGuard
