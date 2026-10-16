#pragma once

#include "platform/CrashGuard.h"
#include <CoreAudio/CoreAudio.h>

namespace CrashGuard
{
// Real device to restore on a crash; kAudioObjectUnknown while not routing. Any thread.
void setRestoreDevice (AudioObjectID);
} // namespace CrashGuard
