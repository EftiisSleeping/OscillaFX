#pragma once

#include "platform/CrashGuard.h"
#include "platform/CrashWatchdog.h"

namespace CrashGuard
{
// Real endpoint (IMMDevice::GetId) to make the default again, for the roles OscillaFX took over
// (CrashWatchdog::RoleBits), if OscillaFX crashes or the session ends while routing; nullptr while
// not routing. Any thread. Copied into a preallocated buffer.
void setRestoreEndpoint (const wchar_t* endpointId, int roles = CrashWatchdog::DEFAULT_ROLES);
} // namespace CrashGuard
