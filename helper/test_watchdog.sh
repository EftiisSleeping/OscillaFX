#!/bin/bash
# Dry-run tests for OscillaWatchdog: a stand-in "app" spawns the helper, writes a restore record
# and is SIGKILLed. The default output device is never changed (OSCILLA_WATCHDOG_DRYRUN=1).
# Usage: helper/test_watchdog.sh <path to OscillaWatchdog>
set -uo pipefail

WATCHDOG="${1:?usage: $0 <path to OscillaWatchdog>}"
TMP="$(mktemp -d)"
RECORD="$TMP/restore_output_uid.txt"
LOG="$HOME/Library/Logs/Oscilla/watchdog.log"
SAVED_UID="BuiltInSpeakerDevice"
export OSCILLA_WATCHDOG_DRYRUN=1
failures=0

log_lines() { [ -f "$LOG" ] && wc -l < "$LOG" || echo 0; }
default_output() { system_profiler SPAudioDataType | grep -B4 "Default Output Device: Yes" | head -1; }

report() { # name, expected log text, log line count before the case
    if tail -n +$(($3 + 1)) "$LOG" | grep -q "$2"; then
        echo "[PASS] $1"
    else
        echo "[FAIL] $1 (expected: $2)"
        failures=$((failures + 1))
    fi
}

# name, expected log text, record ("%PID%" = stand-in pid; empty = none written)
crash_case() {
    local before; before=$(log_lines)
    rm -f "$RECORD"
    bash -c '"$1" $$ "$2" & exec sleep 30' _ "$WATCHDOG" "$RECORD" >/dev/null 2>&1 &
    local app=$!
    sleep 0.5
    [ -n "$3" ] && printf '%s' "${3//%PID%/$app}" > "$RECORD"
    kill -9 "$app"
    wait "$app" 2>/dev/null
    sleep 0.5
    report "$1" "$2" "$before"
}

not_my_parent_case() {
    local before; before=$(log_lines)
    sleep 30 &
    local stranger=$!
    "$WATCHDOG" "$stranger" "$RECORD"
    local status=$?
    { kill "$stranger"; wait "$stranger"; } 2>/dev/null
    [ "$status" -eq 1 ] || { echo "[FAIL] not my parent: exit $status"; failures=$((failures + 1)); }
    report "pid that is not the parent is refused" "is not my parent" "$before"
}

initial_default="$(default_output)"

OSCILLA_WATCHDOG_DRYRUN_DEFAULT_UID=Oscilla_UID \
    crash_case "saved device gone -> other real output" "would set default output to" $'%PID%\nNo_Such_Device_UID\n'
# Machines without built-in speakers (CI VMs): use the real output the fallback just picked.
fallback_uid="$(grep -o 'would set default output to [^ ]*' "$LOG" | tail -1 | awk '{print $NF}')"
if ! system_profiler SPAudioDataType | grep -q "Speakers:" && [ -n "$fallback_uid" ]; then
    SAVED_UID="$fallback_uid"
fi
OSCILLA_WATCHDOG_DRYRUN_DEFAULT_UID=Oscilla_UID \
    crash_case "crash restores saved device" "would set default output to $SAVED_UID " $'%PID%\n'"$SAVED_UID"$'\n'
# Pretend the default is a real device: hermetic even while OscillaFX itself runs and owns the real default.
OSCILLA_WATCHDOG_DRYRUN_DEFAULT_UID="$SAVED_UID" \
    crash_case "default not the virtual device -> untouched" "not the virtual device; nothing to do" $'%PID%\n'"$SAVED_UID"$'\n'
crash_case "clean stop (record removed)" "exited cleanly" ""
crash_case "record of another pid ignored" "ignoring restore file of pid 1" $'1\n'"$SAVED_UID"$'\n'
crash_case "legacy single-line record ignored" "old or unknown format" "$SAVED_UID"
not_my_parent_case

[ "$(default_output)" = "$initial_default" ] || { echo "[FAIL] default output changed!"; failures=$((failures + 1)); }
rm -rf "$TMP"
echo "$([ $failures -eq 0 ] && echo "ALL PASS" || echo "FAILED: $failures")"
exit $((failures > 0))
