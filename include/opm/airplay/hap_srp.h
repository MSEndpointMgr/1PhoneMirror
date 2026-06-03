#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opm::airplay {

// SRP-6a verifier-style server for HomeKit Accessory Protocol pair-setup.
//   Group:    RFC 5054 / RFC 3526 group 15 (3072-bit modulus, g=5)
//   Hash:     SHA-512
//   Username: "Pair-Setup"  (HAP-specified, fixed)
//   Password: 8-digit setup code in "XXX-XX-XXX" format
//
// One instance handles one pair-setup attempt. After verify_client_proof()
// returns the server proof M2, session_key() contains K = SHA-512(S) used to
// derive the ChaCha20-Poly1305 keys for the subsequent encrypted sub-TLVs.
//
// All large numbers are exchanged on the wire as big-endian byte strings; the
// returned/accepted buffers here use the same convention. B and A are 384
// bytes (3072 bits / 8), salt is 16 bytes, M1/M2/K are 64 bytes (SHA-512).

class HapSrpServer {
public:
    HapSrpServer();
    ~HapSrpServer();

    HapSrpServer(const HapSrpServer&) = delete;
    HapSrpServer& operator=(const HapSrpServer&) = delete;

    // Start a session with the given setup code (already in "031-45-154" form,
    // or any UTF-8 password — the value is hashed verbatim).
    // Generates a fresh 16-byte salt and 384-byte private/public pair (b, B).
    // On success out_salt is 16 bytes and out_B is 384 bytes.
    bool start(const std::string& setup_code,
               std::vector<uint8_t>& out_salt,
               std::vector<uint8_t>& out_B);

    // Process the client's A and M1 proof. On success returns the 64-byte
    // server proof M2 and stashes the session key K (SHA-512(S)).
    // Returns empty vector on either decode failure or M1 mismatch — caller
    // must treat that as kTLVError_Authentication and abort.
    std::vector<uint8_t> verify_client_proof(const std::vector<uint8_t>& A,
                                              const std::vector<uint8_t>& M1);

    // Returns the 64-byte session key K (= SHA-512(S)). Valid only after a
    // successful verify_client_proof().
    const std::vector<uint8_t>& session_key() const { return K_; }

private:
    struct Impl;
    Impl* impl_;
    std::vector<uint8_t> K_;
};

bool hap_srp_self_test();

} // namespace opm::airplay
