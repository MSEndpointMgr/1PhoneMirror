#pragma once

// AirPlay HomeKit (HAP) device long-term identity.
//
// Each AirPlay receiver advertises a stable per-installation:
//   - "pi" (Pairing Identifier)   — 17-char MAC-style string in mDNS TXT
//   - "pk" (Long-term Public Key) — Ed25519 32-byte public key, hex in TXT
//
// The matching Ed25519 private key signs proofs during /pair-setup and
// /pair-verify. Both must survive restarts (paired iPads remember pk; if
// it changes they refuse to re-verify until repaired).
//
// Persisted to %APPDATA%\1PhoneMirror\hap_device.key as plain key=value
// lines so the user can inspect/delete to force re-pair.

#include <cstdint>
#include <string>
#include <vector>

namespace opm::airplay {

class HapDevice {
public:
    // Load existing key from APPDATA, or generate + persist a new one on
    // first run. Returns false only on filesystem / OpenSSL hard errors.
    bool load_or_create();

    // 17-byte UTF-8 pairing identifier, e.g. "4a:9c:2f:11:88:b0".
    // Stable across restarts (random on first run, locally-administered MAC
    // bit set so it can't collide with a real NIC).
    const std::string& pi() const { return pi_; }

    // 32-byte Ed25519 long-term public key (what goes into the mDNS "pk"
    // TXT record, lowercased hex).
    const std::vector<uint8_t>& ltpk() const { return ltpk_; }

    // Hex form of ltpk(), 64 chars lowercase — ready to drop into TXT.
    std::string ltpk_hex() const;

    // Ed25519 sign with the long-term private key. Returns 64-byte signature
    // or empty vector on failure.
    std::vector<uint8_t> sign(const uint8_t* msg, size_t len) const;
    std::vector<uint8_t> sign(const std::vector<uint8_t>& msg) const {
        return sign(msg.data(), msg.size());
    }

    // Path of the persisted key file (for diagnostics / unpair-all UI).
    static std::string key_file_path();

private:
    std::string pi_;
    std::vector<uint8_t> ltsk_seed_;   // 32 bytes (Ed25519 seed)
    std::vector<uint8_t> ltpk_;        // 32 bytes
};

bool hap_device_self_test();

} // namespace opm::airplay
