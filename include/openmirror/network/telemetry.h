#pragma once

#include <string>

namespace openmirror::network {

// Where /ping is posted. Override at compile time with
// -DOPENMIRROR_TELEMETRY_URL="https://..." if you ever repoint the backend.
#ifndef OPENMIRROR_TELEMETRY_URL
#define OPENMIRROR_TELEMETRY_URL "https://func-3qykfcznbey62.azurewebsites.net"
#endif

// Returns a stable per-install GUID, stored in
// %LOCALAPPDATA%\1PhoneMirror\install_id.txt. Creates the file on first
// call. Uninstall + reinstall = new ID by design (it is NOT a user id).
// Returns empty on error.
std::string install_id();

// Fire-and-forget anonymous launch ping. Spawns a detached worker thread,
// returns immediately. NEVER blocks startup. Bounded by a 3-second total
// timeout inside the worker. All network/parse failures are swallowed.
//
// `enabled` reflects the user's opt-in preference (Settings.telemetry_enabled).
// If false, the function is a no-op — nothing is sent, no thread is spawned.
//
// `app_version` should be a 3- or 4-part dotted version, e.g. "0.3.8".
void send_launch_ping_async(const std::string& app_version, bool enabled);

} // namespace openmirror::network
