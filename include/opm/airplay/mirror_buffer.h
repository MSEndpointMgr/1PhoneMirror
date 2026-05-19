#pragma once

#include <cstdint>
#include <openssl/evp.h>

namespace opm::airplay {

// AES-128-CTR decryption for AirPlay mirror video stream.
// Key derivation:
//   video_key = SHA-512("AirPlayStreamKey" + str(streamConnectionID) + aeskey)[0:16]
//   video_iv  = SHA-512("AirPlayStreamIV"  + str(streamConnectionID) + aeskey)[0:16]
// Where aeskey is the 16-byte key from FairPlay decrypt of ekey.
class MirrorBuffer {
public:
    MirrorBuffer();
    ~MirrorBuffer();

    MirrorBuffer(const MirrorBuffer&) = delete;
    MirrorBuffer& operator=(const MirrorBuffer&) = delete;

    // Store the 16-byte audio AES key from FairPlay decrypt
    void set_aes_key(const uint8_t key[16]);

    // Derive video AES-CTR key/IV from streamConnectionID
    void init_aes(uint64_t stream_connection_id);

    // Decrypt mirror video payload.
    // OpenSSL CTR mode handles partial blocks and state across calls.
    void decrypt(uint8_t* input, uint8_t* output, int len);

    bool is_initialized() const { return ctx_ != nullptr; }

private:
    EVP_CIPHER_CTX* ctx_ = nullptr;
    uint8_t aes_key_[16] = {};
};

} // namespace opm::airplay
