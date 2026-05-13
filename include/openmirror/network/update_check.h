#pragma once

#include <string>

namespace openmirror::network {

struct UpdateCheckResult {
    bool        ok = false;             // true if the network call succeeded
    bool        update_available = false; // true if latest > current
    std::string current_version;        // e.g. "0.3.4"
    std::string latest_version;         // e.g. "0.3.5" (empty if !ok)
    std::string release_url;            // browser URL for the new release
    std::string error;                  // human-readable failure reason
};

// Synchronous GET https://api.github.com/repos/MSEndpointMgr/1PhoneMirror/releases/latest
// Compares the returned tag_name (stripped of any leading 'v') to
// `current_version` using numeric major.minor.patch ordering.
// Returns ok=false on any network/parse failure (silent — caller decides
// whether to surface the error).
UpdateCheckResult check_for_update(const std::string& current_version);

} // namespace openmirror::network
