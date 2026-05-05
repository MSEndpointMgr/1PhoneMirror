#pragma once

#include <cstdint>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;

namespace openmirror::airplay {

// Ed25519 / X25519 key exchange for AirPlay pair-setup and pair-verify
class Pairing {
public:
    Pairing();
    ~Pairing();

    Pairing(const Pairing&) = delete;
    Pairing& operator=(const Pairing&) = delete;

    // Generate persistent Ed25519 keypair. Call once at startup.
    bool init();

    // Get our Ed25519 public key (32 bytes)
    std::vector<uint8_t> get_public_key() const;

    // pair-setup: receive client's 32-byte key, return our 32-byte public key
    std::vector<uint8_t> pair_setup(const uint8_t* client_pk, int len);

    // pair-verify phase 1 (data[0]==1, 68 bytes total)
    // Returns X25519 pubkey (32) + encrypted signature (64) = 96 bytes
    std::vector<uint8_t> pair_verify_phase1(const uint8_t* data, int len);

    // pair-verify phase 2 (data[0]==0, 68 bytes total)
    // Returns true on success
    bool pair_verify_phase2(const uint8_t* data, int len);

    // Get ECDH shared secret (32 bytes, for SETUP stream encryption)
    bool get_ecdh_secret(uint8_t secret[32]) const;

private:
    void reset_session();
    bool derive_key(const char* salt, int salt_len, uint8_t* out, int out_len);

    EVP_PKEY* ed_key_ = nullptr;        // Our persistent Ed25519 keypair

    // Session state (reset per connection)
    EVP_PKEY* ecdh_ours_ = nullptr;
    EVP_PKEY* ecdh_theirs_ = nullptr;
    EVP_PKEY* ed_theirs_ = nullptr;
    uint8_t ecdh_secret_[32] = {};

    enum State { Initial, Setup, Handshake, Finished };
    State state_ = Initial;
};

// FairPlay DRM handshake for AirPlay (fp-setup)
class FairPlay {
public:
    FairPlay() = default;

    // fp-setup phase 1: 16-byte request → 142-byte response
    std::vector<uint8_t> setup(const uint8_t* req, int len);

    // fp-setup phase 2: 164-byte request → 32-byte response
    std::vector<uint8_t> handshake(const uint8_t* req, int len);

    // Decrypt AES key from ekey (72 bytes in) → 16-byte key out
    // Returns false if handshake hasn't completed
    bool decrypt(const uint8_t* input, uint8_t* output);

private:
    uint8_t keymsg_[164] = {};
    int keymsg_len_ = 0;
};

} // namespace openmirror::airplay
