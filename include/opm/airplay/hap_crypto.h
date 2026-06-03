#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Crypto primitives used by the HomeKit Accessory Protocol (HAP) pair-setup
// and pair-verify flows. Thin wrappers over OpenSSL EVP.

namespace opm::airplay {

// HKDF-SHA512 → out_len bytes. Returns empty vector on failure.
std::vector<uint8_t> hkdf_sha512(const uint8_t* salt, size_t salt_len,
                                 const uint8_t* ikm,  size_t ikm_len,
                                 const uint8_t* info, size_t info_len,
                                 size_t out_len);

// Convenience overload: string salt + string info.
std::vector<uint8_t> hkdf_sha512(const std::string& salt,
                                 const std::vector<uint8_t>& ikm,
                                 const std::string& info,
                                 size_t out_len = 32);

// ChaCha20-Poly1305 AEAD (RFC 7539, 12-byte nonce, 16-byte tag).
// On encrypt: output = ciphertext (plain_len bytes) + 16-byte Poly1305 tag.
// On decrypt: input must include the trailing 16-byte tag; output is plaintext
//             on success, empty vector on tag-mismatch / failure.
std::vector<uint8_t> chacha20_poly1305_encrypt(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aad_len,
    const uint8_t* plain, size_t plain_len);

std::vector<uint8_t> chacha20_poly1305_decrypt(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aad_len,
    const uint8_t* cipher_and_tag, size_t cipher_and_tag_len);

// HAP commonly uses an 8-byte nonce zero-padded on the left into a 12-byte
// IETF ChaCha20-Poly1305 nonce. Helper builds the nonce from an ASCII tag
// like "PS-Msg05" — used directly as the 8-byte trailing portion.
void hap_nonce_from_tag(const char* tag8, uint8_t nonce[12]);

bool hap_crypto_self_test();

} // namespace opm::airplay
