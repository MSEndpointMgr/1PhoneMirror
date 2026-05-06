#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openmirror::airplay {

// SRP-6a server for AirPlay 1 PIN pairing.
// Uses RFC 5054 Group 14 (2048-bit), SHA-1 hashing — what classic
// AirPlay receivers (Apple TV 2/3, AirPort Express) use for the
// /pair-pin-start + /pair-setup-pin handshake.
//
// Flow:
//   1) start(pin)                 — host generates s, b; computes v, B
//   2) get_salt() / get_B()       — sent to client in step-1 response
//   3) process_client_pubkey(A,M1) — verify client proof, compute K, M2
//   4) get_M2()                   — sent in step-2 response
//   5) get_session_key()          — for AES-CBC encryption in step-3
//
// On any failure, methods return empty/false and the session is invalid.
class SrpPinServer {
public:
    SrpPinServer();
    ~SrpPinServer();

    SrpPinServer(const SrpPinServer&) = delete;
    SrpPinServer& operator=(const SrpPinServer&) = delete;

    // Start a new SRP session with the given 4-digit PIN string.
    // The username (SRP "I") defaults to "Pair-Setup" but may be overridden
    // \u2014 iOS clients send their device MAC address as the username.
    bool start(const std::string& pin, const std::string& username = "Pair-Setup");

    // 16-byte random salt (s), generated in start().
    std::vector<uint8_t> get_salt() const;

    // Server public value B (256 bytes, big-endian, zero-padded).
    std::vector<uint8_t> get_B() const;

    // Process client public value A (256 bytes) and proof M1 (20 bytes).
    // Returns true if M1 verifies. Computes shared K and server proof M2.
    bool process_client_pubkey(const std::vector<uint8_t>& A,
                               const std::vector<uint8_t>& M1);

    // 20-byte server proof M2 — sent after successful M1 verification.
    std::vector<uint8_t> get_M2() const;

    // 64-byte SHA1-interleaved session key K — used to derive AES-CBC
    // key/IV for the encrypted-LTPK exchange in pair-setup-pin step 3.
    std::vector<uint8_t> get_session_key() const;

    bool is_authenticated() const { return authenticated_; }

private:
    struct Impl;
    Impl* impl_;
    bool authenticated_ = false;
};

// Decrypt the encrypted LTPK payload (epk + authTag) sent by the client
// in pair-setup-pin step 3, using AES-128-CBC with key/IV derived from K.
// Returns the 32-byte client Ed25519 public key on success.
//
// session_K must be the 64-byte interleaved key from SrpPinServer::get_session_key().
std::vector<uint8_t> srp_pin_decrypt_ltpk(const std::vector<uint8_t>& session_K,
                                          const std::vector<uint8_t>& epk);

// Encrypt our 32-byte Ed25519 LTPK for the response in pair-setup-pin step 3.
std::vector<uint8_t> srp_pin_encrypt_ltpk(const std::vector<uint8_t>& session_K,
                                          const std::vector<uint8_t>& our_ltpk);

} // namespace openmirror::airplay
