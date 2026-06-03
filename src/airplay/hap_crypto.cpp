#include <opm/airplay/hap_crypto.h>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/err.h>

#include <cstring>
#include <iostream>

namespace opm::airplay {

// ----------------------------------------------------------------------------
// HKDF-SHA512
// ----------------------------------------------------------------------------

std::vector<uint8_t> hkdf_sha512(const uint8_t* salt, size_t salt_len,
                                 const uint8_t* ikm,  size_t ikm_len,
                                 const uint8_t* info, size_t info_len,
                                 size_t out_len) {
    std::vector<uint8_t> out(out_len);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return {};

    bool ok =
        EVP_PKEY_derive_init(ctx) == 1 &&
        EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512()) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, static_cast<int>(salt_len)) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_key (ctx, ikm,  static_cast<int>(ikm_len )) == 1 &&
        EVP_PKEY_CTX_add1_hkdf_info(ctx, info, static_cast<int>(info_len)) == 1;

    size_t n = out_len;
    if (ok) ok = (EVP_PKEY_derive(ctx, out.data(), &n) == 1 && n == out_len);
    EVP_PKEY_CTX_free(ctx);
    if (!ok) return {};
    return out;
}

std::vector<uint8_t> hkdf_sha512(const std::string& salt,
                                 const std::vector<uint8_t>& ikm,
                                 const std::string& info,
                                 size_t out_len) {
    return hkdf_sha512(reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                       ikm.data(), ikm.size(),
                       reinterpret_cast<const uint8_t*>(info.data()), info.size(),
                       out_len);
}

// ----------------------------------------------------------------------------
// ChaCha20-Poly1305 (IETF, 12-byte nonce, 16-byte tag)
// ----------------------------------------------------------------------------

std::vector<uint8_t> chacha20_poly1305_encrypt(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aad_len,
    const uint8_t* plain, size_t plain_len) {

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    std::vector<uint8_t> out(plain_len + 16);
    int outl = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;

    if (ok && aad_len) {
        ok = EVP_EncryptUpdate(ctx, nullptr, &outl, aad, static_cast<int>(aad_len)) == 1;
    }
    if (ok && plain_len) {
        ok = EVP_EncryptUpdate(ctx, out.data(), &outl, plain, static_cast<int>(plain_len)) == 1
             && outl == static_cast<int>(plain_len);
    }
    int finl = 0;
    if (ok) ok = EVP_EncryptFinal_ex(ctx, out.data() + plain_len, &finl) == 1 && finl == 0;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                                     out.data() + plain_len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    return out;
}

std::vector<uint8_t> chacha20_poly1305_decrypt(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, size_t aad_len,
    const uint8_t* cipher_and_tag, size_t cipher_and_tag_len) {

    if (cipher_and_tag_len < 16) return {};
    size_t cipher_len = cipher_and_tag_len - 16;
    const uint8_t* tag = cipher_and_tag + cipher_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    std::vector<uint8_t> out(cipher_len);
    int outl = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;

    if (ok && aad_len) {
        ok = EVP_DecryptUpdate(ctx, nullptr, &outl, aad, static_cast<int>(aad_len)) == 1;
    }
    if (ok && cipher_len) {
        ok = EVP_DecryptUpdate(ctx, out.data(), &outl,
                               cipher_and_tag, static_cast<int>(cipher_len)) == 1
             && outl == static_cast<int>(cipher_len);
    }
    if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                     const_cast<uint8_t*>(tag)) == 1;
    int finl = 0;
    if (ok) ok = EVP_DecryptFinal_ex(ctx, out.data() + cipher_len, &finl) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    return out;
}

void hap_nonce_from_tag(const char* tag8, uint8_t nonce[12]) {
    std::memset(nonce, 0, 12);
    // HAP tags are 8 ASCII chars placed in the last 8 bytes of the 12-byte
    // nonce (RFC 7539 layout: 4-byte counter prefix is implicit zero).
    size_t n = std::strlen(tag8);
    if (n > 8) n = 8;
    std::memcpy(nonce + 4, tag8, n);
}

// ----------------------------------------------------------------------------
// Self-test
// ----------------------------------------------------------------------------

bool hap_crypto_self_test() {
    // HKDF-SHA512 structural tests. (RFC 5869 only publishes SHA-256/SHA-1
    // vectors, so we exercise the wrapper's contract instead of a magic
    // number: determinism, output length, and sensitivity to every input.)
    static const uint8_t ikm[22] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
    static const uint8_t salt[13] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c };
    static const uint8_t info[10] = {
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9 };

    auto base = hkdf_sha512(salt, sizeof(salt), ikm, sizeof(ikm),
                            info, sizeof(info), 32);
    if (base.size() != 32) {
        std::cerr << "[HAP-CRYPTO] HKDF wrong output length\n"; return false;
    }
    // Determinism.
    auto rerun = hkdf_sha512(salt, sizeof(salt), ikm, sizeof(ikm),
                              info, sizeof(info), 32);
    if (rerun != base) {
        std::cerr << "[HAP-CRYPTO] HKDF non-deterministic\n"; return false;
    }
    // Variable output length.
    auto sixtyfour = hkdf_sha512(salt, sizeof(salt), ikm, sizeof(ikm),
                                  info, sizeof(info), 64);
    if (sixtyfour.size() != 64 ||
        std::memcmp(sixtyfour.data(), base.data(), 32) != 0) {
        // HKDF-Expand is a prefix construction — the first 32 bytes of a
        // 64-byte derivation must equal the standalone 32-byte derivation.
        std::cerr << "[HAP-CRYPTO] HKDF length-extension prefix property failed\n";
        return false;
    }
    // Sensitivity to each input.
    uint8_t ikm2[22]; std::memcpy(ikm2, ikm, 22); ikm2[0] ^= 1;
    if (hkdf_sha512(salt, 13, ikm2, 22, info, 10, 32) == base) {
        std::cerr << "[HAP-CRYPTO] HKDF ignored ikm change\n"; return false;
    }
    uint8_t salt2[13]; std::memcpy(salt2, salt, 13); salt2[5] ^= 1;
    if (hkdf_sha512(salt2, 13, ikm, 22, info, 10, 32) == base) {
        std::cerr << "[HAP-CRYPTO] HKDF ignored salt change\n"; return false;
    }
    uint8_t info2[10]; std::memcpy(info2, info, 10); info2[9] ^= 1;
    if (hkdf_sha512(salt, 13, ikm, 22, info2, 10, 32) == base) {
        std::cerr << "[HAP-CRYPTO] HKDF ignored info change\n"; return false;
    }

    // ChaCha20-Poly1305 round-trip with AAD.
    uint8_t key[32];   for (int i = 0; i < 32; ++i) key[i]   = static_cast<uint8_t>(i);
    uint8_t nonce[12]; for (int i = 0; i < 12; ++i) nonce[i] = static_cast<uint8_t>(i + 0x40);
    const char* aad   = "associated-data";
    const char* plain = "the quick brown fox jumps over the lazy dog";

    auto ct = chacha20_poly1305_encrypt(
        key, nonce,
        reinterpret_cast<const uint8_t*>(aad), std::strlen(aad),
        reinterpret_cast<const uint8_t*>(plain), std::strlen(plain));
    if (ct.size() != std::strlen(plain) + 16) {
        std::cerr << "[HAP-CRYPTO] chacha20-poly1305 encrypt wrong size\n";
        return false;
    }
    auto pt = chacha20_poly1305_decrypt(
        key, nonce,
        reinterpret_cast<const uint8_t*>(aad), std::strlen(aad),
        ct.data(), ct.size());
    if (pt.size() != std::strlen(plain) ||
        std::memcmp(pt.data(), plain, pt.size()) != 0) {
        std::cerr << "[HAP-CRYPTO] chacha20-poly1305 round-trip failed\n";
        return false;
    }

    // Tamper-detection: flipping one ciphertext byte must fail.
    ct[0] ^= 0x01;
    auto bad = chacha20_poly1305_decrypt(
        key, nonce,
        reinterpret_cast<const uint8_t*>(aad), std::strlen(aad),
        ct.data(), ct.size());
    if (!bad.empty()) {
        std::cerr << "[HAP-CRYPTO] tag did not detect tampered ciphertext\n";
        return false;
    }

    // hap_nonce_from_tag: "PS-Msg05" → 00000000 50532D4D 73673035
    uint8_t n12[12];
    hap_nonce_from_tag("PS-Msg05", n12);
    static const uint8_t expect_n[12] = {
        0,0,0,0, 'P','S','-','M','s','g','0','5' };
    if (std::memcmp(n12, expect_n, 12) != 0) {
        std::cerr << "[HAP-CRYPTO] hap_nonce_from_tag layout wrong\n";
        return false;
    }

    std::cerr << "[HAP-CRYPTO] PASS\n";
    return true;
}

} // namespace opm::airplay
