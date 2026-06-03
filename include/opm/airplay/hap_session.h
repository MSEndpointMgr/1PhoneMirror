#pragma once

#include <cstdint>
#include <vector>

namespace opm::airplay {

// HomeKit Accessory Protocol (HAP) §5.5.2 encrypted frame codec.
//
// After /pair-verify M4 the RTSP control TCP stream is wrapped in a per-frame
// ChaCha20-Poly1305 AEAD. Each direction owns its own 64-bit counter and
// 32-byte key derived in pair-verify:
//   server\xE2\x86\x92client (write):  HKDF-SHA512(salt="Control-Salt",
//                              info="Control-Read-Encryption-Key")
//   client\xE2\x86\x92server (read):   HKDF-SHA512(salt="Control-Salt",
//                              info="Control-Write-Encryption-Key")
//
// Frame on the wire:
//   [ 2-byte LE length L (AAD) ][ L bytes ciphertext ][ 16-byte Poly1305 tag ]
// Nonce: 4 zero bytes || 8-byte LE counter. Counter increments after each
// frame, separately per direction. Max plaintext per frame is 1024 bytes.

class HapSession {
public:
    HapSession() = default;
    HapSession(const std::vector<uint8_t>& read_key,
               const std::vector<uint8_t>& write_key);

    bool valid() const { return read_key_.size() == 32 && write_key_.size() == 32; }

    // Max plaintext bytes carried in one frame (HAP cap).
    static constexpr size_t kMaxFrame = 1024;

    // Encrypt up to kMaxFrame plaintext bytes into a single wire frame
    // (2B length + ciphertext + 16B tag). Returns empty on failure.
    std::vector<uint8_t> encrypt_frame(const uint8_t* plain, size_t len);
    std::vector<uint8_t> encrypt_frame(const std::vector<uint8_t>& plain) {
        return encrypt_frame(plain.data(), plain.size());
    }

    // Decrypt one wire frame in-place from `wire` (must be 2 + L + 16 bytes).
    // Returns the plaintext, or empty on AEAD/length failure.
    std::vector<uint8_t> decrypt_frame(const uint8_t* wire, size_t wire_len);
    std::vector<uint8_t> decrypt_frame(const std::vector<uint8_t>& wire) {
        return decrypt_frame(wire.data(), wire.size());
    }

    uint64_t read_counter()  const { return read_counter_; }
    uint64_t write_counter() const { return write_counter_; }

private:
    std::vector<uint8_t> read_key_;
    std::vector<uint8_t> write_key_;
    uint64_t read_counter_  = 0;
    uint64_t write_counter_ = 0;
};

bool hap_session_self_test();

} // namespace opm::airplay
