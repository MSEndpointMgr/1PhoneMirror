#include <openmirror/airplay/pairing.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <cstring>
#include <iostream>
#include <iomanip>

extern "C" {
    void playfair_decrypt(unsigned char* message3, unsigned char* cipherText, unsigned char* keyOut);
}

namespace openmirror::airplay {

static constexpr int ED25519_KEY_SIZE = 32;
static constexpr int X25519_KEY_SIZE = 32;
static constexpr int PAIRING_SIG_SIZE = 64; // Ed25519 signature size
static constexpr int AES_BLOCK_SIZE = 16;

// ===== Pairing =====

Pairing::Pairing() = default;

Pairing::~Pairing() {
    reset_session();
    if (ed_key_) EVP_PKEY_free(ed_key_);
}

bool Pairing::init() {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!pctx) return false;

    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_keygen(pctx, &ed_key_) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);
    return true;
}

std::vector<uint8_t> Pairing::get_public_key() const {
    std::vector<uint8_t> pk(ED25519_KEY_SIZE);
    size_t len = ED25519_KEY_SIZE;
    if (!ed_key_ || !EVP_PKEY_get_raw_public_key(ed_key_, pk.data(), &len)) {
        return {};
    }
    return pk;
}

std::vector<uint8_t> Pairing::pair_setup(const uint8_t* client_pk, int len) {
    if (len != ED25519_KEY_SIZE) {
        std::cerr << "[Pairing] Invalid pair-setup data length: " << len << "\n";
        return {};
    }

    reset_session();
    state_ = Setup;

    auto pk = get_public_key();
    std::cout << "[Pairing] pair-setup: returning Ed25519 public key (" << pk.size() << " bytes)\n";
    return pk;
}

std::vector<uint8_t> Pairing::pair_verify_phase1(const uint8_t* data, int len) {
    // data: [01 00 00 00] [32 bytes X25519 pubkey] [32 bytes Ed25519 pubkey]
    if (len != 4 + X25519_KEY_SIZE + ED25519_KEY_SIZE) {
        std::cerr << "[Pairing] Invalid pair-verify phase 1 data length: " << len << "\n";
        return {};
    }
    if (state_ != Setup) {
        std::cerr << "[Pairing] pair-verify phase 1: not in Setup state\n";
        return {};
    }

    const uint8_t* client_x25519_pk = data + 4;
    const uint8_t* client_ed25519_pk = data + 4 + X25519_KEY_SIZE;

    // Store client's Ed25519 public key
    ed_theirs_ = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              client_ed25519_pk, ED25519_KEY_SIZE);
    if (!ed_theirs_) {
        std::cerr << "[Pairing] Failed to load client Ed25519 key\n";
        return {};
    }

    // Store client's X25519 public key
    ecdh_theirs_ = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                client_x25519_pk, X25519_KEY_SIZE);
    if (!ecdh_theirs_) {
        std::cerr << "[Pairing] Failed to load client X25519 key\n";
        return {};
    }

    // Generate our X25519 keypair
    {
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        if (!pctx || EVP_PKEY_keygen_init(pctx) <= 0 ||
            EVP_PKEY_keygen(pctx, &ecdh_ours_) <= 0) {
            if (pctx) EVP_PKEY_CTX_free(pctx);
            std::cerr << "[Pairing] Failed to generate X25519 keypair\n";
            return {};
        }
        EVP_PKEY_CTX_free(pctx);
    }

    // Derive ECDH shared secret
    {
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new(ecdh_ours_, nullptr);
        if (!pctx || EVP_PKEY_derive_init(pctx) <= 0 ||
            EVP_PKEY_derive_set_peer(pctx, ecdh_theirs_) <= 0) {
            if (pctx) EVP_PKEY_CTX_free(pctx);
            std::cerr << "[Pairing] Failed to derive ECDH secret\n";
            return {};
        }
        size_t secret_len = X25519_KEY_SIZE;
        if (EVP_PKEY_derive(pctx, ecdh_secret_, &secret_len) <= 0) {
            EVP_PKEY_CTX_free(pctx);
            std::cerr << "[Pairing] ECDH derive failed\n";
            return {};
        }
        EVP_PKEY_CTX_free(pctx);
    }

    // Get our X25519 public key
    uint8_t our_x25519_pk[X25519_KEY_SIZE];
    {
        size_t len = X25519_KEY_SIZE;
        if (!EVP_PKEY_get_raw_public_key(ecdh_ours_, our_x25519_pk, &len)) {
            std::cerr << "[Pairing] Failed to get X25519 public key\n";
            return {};
        }
    }

    // Sign: our_x25519_pk + their_x25519_pk (64 bytes of message)
    uint8_t sig_msg[PAIRING_SIG_SIZE];
    memcpy(sig_msg, our_x25519_pk, X25519_KEY_SIZE);
    memcpy(sig_msg + X25519_KEY_SIZE, client_x25519_pk, X25519_KEY_SIZE);

    uint8_t signature[PAIRING_SIG_SIZE];
    {
        EVP_MD_CTX* mctx = EVP_MD_CTX_new();
        if (!mctx) return {};

        size_t sig_len = PAIRING_SIG_SIZE;
        if (EVP_DigestSignInit(mctx, nullptr, nullptr, nullptr, ed_key_) <= 0 ||
            EVP_DigestSign(mctx, signature, &sig_len, sig_msg, PAIRING_SIG_SIZE) <= 0) {
            EVP_MD_CTX_free(mctx);
            std::cerr << "[Pairing] Ed25519 signing failed\n";
            return {};
        }
        EVP_MD_CTX_free(mctx);
    }

    // Encrypt signature with AES-CTR using keys derived from shared secret
    uint8_t aes_key[AES_BLOCK_SIZE];
    uint8_t aes_iv[AES_BLOCK_SIZE];
    if (!derive_key("Pair-Verify-AES-Key", 19, aes_key, AES_BLOCK_SIZE) ||
        !derive_key("Pair-Verify-AES-IV", 18, aes_iv, AES_BLOCK_SIZE)) {
        std::cerr << "[Pairing] Key derivation failed\n";
        return {};
    }

    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return {};
        EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, aes_key, aes_iv);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        int out_len = 0;
        EVP_EncryptUpdate(ctx, signature, &out_len, signature, PAIRING_SIG_SIZE);
        EVP_CIPHER_CTX_free(ctx);
    }

    state_ = Handshake;

    // Response: our X25519 pubkey (32) + encrypted signature (64) = 96 bytes
    std::vector<uint8_t> response(X25519_KEY_SIZE + PAIRING_SIG_SIZE);
    memcpy(response.data(), our_x25519_pk, X25519_KEY_SIZE);
    memcpy(response.data() + X25519_KEY_SIZE, signature, PAIRING_SIG_SIZE);

    std::cout << "[Pairing] pair-verify phase 1: ECDH + signature (" << response.size() << " bytes)\n";
    return response;
}

bool Pairing::pair_verify_phase2(const uint8_t* data, int len) {
    // data: [00 00 00 00] [64 bytes encrypted signature]
    if (len != 4 + PAIRING_SIG_SIZE) {
        std::cerr << "[Pairing] Invalid pair-verify phase 2 data length: " << len << "\n";
        return false;
    }
    if (state_ != Handshake) {
        std::cerr << "[Pairing] pair-verify phase 2: not in Handshake state\n";
        return false;
    }

    const uint8_t* encrypted_sig = data + 4;

    // Derive AES key/IV from shared secret
    uint8_t aes_key[AES_BLOCK_SIZE];
    uint8_t aes_iv[AES_BLOCK_SIZE];
    if (!derive_key("Pair-Verify-AES-Key", 19, aes_key, AES_BLOCK_SIZE) ||
        !derive_key("Pair-Verify-AES-IV", 18, aes_iv, AES_BLOCK_SIZE)) {
        return false;
    }

    // Decrypt: first do a "fake round" (matching the encryption in phase 1),
    // then decrypt the client's signature
    uint8_t sig_buffer[PAIRING_SIG_SIZE];
    uint8_t decrypted[PAIRING_SIG_SIZE];
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;
        EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, aes_key, aes_iv);
        EVP_CIPHER_CTX_set_padding(ctx, 0);

        // Fake round: encrypt 64 zeros to advance the CTR counter
        // (matches the encryption we did in phase 1)
        int out_len = 0;
        memset(sig_buffer, 0, PAIRING_SIG_SIZE);
        EVP_EncryptUpdate(ctx, sig_buffer, &out_len, sig_buffer, PAIRING_SIG_SIZE);

        // Now decrypt the client's signature
        EVP_EncryptUpdate(ctx, decrypted, &out_len, encrypted_sig, PAIRING_SIG_SIZE);
        EVP_CIPHER_CTX_free(ctx);
    }

    // Verify: their_x25519_pk + our_x25519_pk
    uint8_t sig_msg[PAIRING_SIG_SIZE];
    {
        size_t klen = X25519_KEY_SIZE;
        EVP_PKEY_get_raw_public_key(ecdh_theirs_, sig_msg, &klen);
        EVP_PKEY_get_raw_public_key(ecdh_ours_, sig_msg + X25519_KEY_SIZE, &klen);
    }

    // Verify Ed25519 signature
    {
        EVP_MD_CTX* mctx = EVP_MD_CTX_new();
        if (!mctx) return false;

        if (EVP_DigestVerifyInit(mctx, nullptr, nullptr, nullptr, ed_theirs_) <= 0) {
            EVP_MD_CTX_free(mctx);
            std::cerr << "[Pairing] DigestVerifyInit failed\n";
            return false;
        }

        int ret = EVP_DigestVerify(mctx, decrypted, PAIRING_SIG_SIZE,
                                   sig_msg, PAIRING_SIG_SIZE);
        EVP_MD_CTX_free(mctx);

        if (ret != 1) {
            std::cerr << "[Pairing] pair-verify phase 2: signature verification FAILED\n";
            return false;
        }
    }

    state_ = Finished;
    std::cout << "[Pairing] pair-verify phase 2: signature verified OK\n";
    return true;
}

bool Pairing::get_ecdh_secret(uint8_t secret[32]) const {
    if (state_ < Handshake) return false;
    memcpy(secret, ecdh_secret_, 32);
    return true;
}

void Pairing::reset_session() {
    if (ecdh_ours_) { EVP_PKEY_free(ecdh_ours_); ecdh_ours_ = nullptr; }
    if (ecdh_theirs_) { EVP_PKEY_free(ecdh_theirs_); ecdh_theirs_ = nullptr; }
    if (ed_theirs_) { EVP_PKEY_free(ed_theirs_); ed_theirs_ = nullptr; }
    memset(ecdh_secret_, 0, sizeof(ecdh_secret_));
    state_ = Initial;
}

bool Pairing::derive_key(const char* salt, int salt_len, uint8_t* out, int out_len) {
    // SHA-512(salt + ecdh_secret), take first out_len bytes
    uint8_t hash[SHA512_DIGEST_LENGTH];

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    if (EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr) <= 0 ||
        EVP_DigestUpdate(ctx, salt, salt_len) <= 0 ||
        EVP_DigestUpdate(ctx, ecdh_secret_, X25519_KEY_SIZE) <= 0) {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) <= 0) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);

    if (out_len > (int)hash_len) return false;
    memcpy(out, hash, out_len);
    return true;
}

// ===== FairPlay =====

// Hardcoded FairPlay response tables (from AirPlay protocol reverse engineering)
// Each mode (0-3) has a 142-byte response for fp-setup phase 1
static const uint8_t reply_message[4][142] = {
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x00,
     0x0f,0x9f,0x3f,0x9e,0x0a,0x25,0x21,0xdb,0xdf,0x31,0x2a,0xb2,0xbf,0xb2,
     0x9e,0x8d,0x23,0x2b,0x63,0x76,0xa8,0xc8,0x18,0x70,0x1d,0x22,0xae,0x93,
     0xd8,0x27,0x37,0xfe,0xaf,0x9d,0xb4,0xfd,0xf4,0x1c,0x2d,0xba,0x9d,0x1f,
     0x49,0xca,0xaa,0xbf,0x65,0x91,0xac,0x1f,0x7b,0xc6,0xf7,0xe0,0x66,0x3d,
     0x21,0xaf,0xe0,0x15,0x65,0x95,0x3e,0xab,0x81,0xf4,0x18,0xce,0xed,0x09,
     0x5a,0xdb,0x7c,0x3d,0x0e,0x25,0x49,0x09,0xa7,0x98,0x31,0xd4,0x9c,0x39,
     0x82,0x97,0x34,0x34,0xfa,0xcb,0x42,0xc6,0x3a,0x1c,0xd9,0x11,0xa6,0xfe,
     0x94,0x1a,0x8a,0x6d,0x4a,0x74,0x3b,0x46,0xc3,0xa7,0x64,0x9e,0x44,0xc7,
     0x89,0x55,0xe4,0x9d,0x81,0x55,0x00,0x95,0x49,0xc4,0xe2,0xf7,0xa3,0xf6,
     0xd5,0xba},
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x01,
     0xcf,0x32,0xa2,0x57,0x14,0xb2,0x52,0x4f,0x8a,0xa0,0xad,0x7a,0xf1,0x64,
     0xe3,0x7b,0xcf,0x44,0x24,0xe2,0x00,0x04,0x7e,0xfc,0x0a,0xd6,0x7a,0xfc,
     0xd9,0x5d,0xed,0x1c,0x27,0x30,0xbb,0x59,0x1b,0x96,0x2e,0xd6,0x3a,0x9c,
     0x4d,0xed,0x88,0xba,0x8f,0xc7,0x8d,0xe6,0x4d,0x91,0xcc,0xfd,0x5c,0x7b,
     0x56,0xda,0x88,0xe3,0x1f,0x5c,0xce,0xaf,0xc7,0x43,0x19,0x95,0xa0,0x16,
     0x65,0xa5,0x4e,0x19,0x39,0xd2,0x5b,0x94,0xdb,0x64,0xb9,0xe4,0x5d,0x8d,
     0x06,0x3e,0x1e,0x6a,0xf0,0x7e,0x96,0x56,0x16,0x2b,0x0e,0xfa,0x40,0x42,
     0x75,0xea,0x5a,0x44,0xd9,0x59,0x1c,0x72,0x56,0xb9,0xfb,0xe6,0x51,0x38,
     0x98,0xb8,0x02,0x27,0x72,0x19,0x88,0x57,0x16,0x50,0x94,0x2a,0xd9,0x46,
     0x68,0x8a},
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x02,
     0xc1,0x69,0xa3,0x52,0xee,0xed,0x35,0xb1,0x8c,0xdd,0x9c,0x58,0xd6,0x4f,
     0x16,0xc1,0x51,0x9a,0x89,0xeb,0x53,0x17,0xbd,0x0d,0x43,0x36,0xcd,0x68,
     0xf6,0x38,0xff,0x9d,0x01,0x6a,0x5b,0x52,0xb7,0xfa,0x92,0x16,0xb2,0xb6,
     0x54,0x82,0xc7,0x84,0x44,0x11,0x81,0x21,0xa2,0xc7,0xfe,0xd8,0x3d,0xb7,
     0x11,0x9e,0x91,0x82,0xaa,0xd7,0xd1,0x8c,0x70,0x63,0xe2,0xa4,0x57,0x55,
     0x59,0x10,0xaf,0x9e,0x0e,0xfc,0x76,0x34,0x7d,0x16,0x40,0x43,0x80,0x7f,
     0x58,0x1e,0xe4,0xfb,0xe4,0x2c,0xa9,0xde,0xdc,0x1b,0x5e,0xb2,0xa3,0xaa,
     0x3d,0x2e,0xcd,0x59,0xe7,0xee,0xe7,0x0b,0x36,0x29,0xf2,0x2a,0xfd,0x16,
     0x1d,0x87,0x73,0x53,0xdd,0xb9,0x9a,0xdc,0x8e,0x07,0x00,0x6e,0x56,0xf8,
     0x50,0xce},
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x03,
     0x90,0x01,0xe1,0x72,0x7e,0x0f,0x57,0xf9,0xf5,0x88,0x0d,0xb1,0x04,0xa6,
     0x25,0x7a,0x23,0xf5,0xcf,0xff,0x1a,0xbb,0xe1,0xe9,0x30,0x45,0x25,0x1a,
     0xfb,0x97,0xeb,0x9f,0xc0,0x01,0x1e,0xbe,0x0f,0x3a,0x81,0xdf,0x5b,0x69,
     0x1d,0x76,0xac,0xb2,0xf7,0xa5,0xc7,0x08,0xe3,0xd3,0x28,0xf5,0x6b,0xb3,
     0x9d,0xbd,0xe5,0xf2,0x9c,0x8a,0x17,0xf4,0x81,0x48,0x7e,0x3a,0xe8,0x63,
     0xc6,0x78,0x32,0x54,0x22,0xe6,0xf7,0x8e,0x16,0x6d,0x18,0xaa,0x7f,0xd6,
     0x36,0x25,0x8b,0xce,0x28,0x72,0x6f,0x66,0x1f,0x73,0x88,0x93,0xce,0x44,
     0x31,0x1e,0x4b,0xe6,0xc0,0x53,0x51,0x93,0xe5,0xef,0x72,0xe8,0x68,0x62,
     0x33,0x72,0x9c,0x22,0x7d,0x82,0x0c,0x99,0x94,0x45,0xd8,0x92,0x46,0xc8,
     0xc3,0x59}
};

// FairPlay handshake response header: "FPLY" + version + type
static const uint8_t fp_header[12] = {
    0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x14
};

std::vector<uint8_t> FairPlay::setup(const uint8_t* req, int len) {
    if (len < 16) {
        std::cerr << "[FairPlay] setup: invalid request length " << len << "\n";
        return {};
    }

    if (req[4] != 0x03) {
        std::cerr << "[FairPlay] setup: unsupported version " << (int)req[4] << "\n";
        return {};
    }

    int mode = req[14];
    if (mode < 0 || mode > 3) {
        std::cerr << "[FairPlay] setup: invalid mode " << mode << "\n";
        return {};
    }

    std::vector<uint8_t> response(142);
    memcpy(response.data(), reply_message[mode], 142);

    keymsg_len_ = 0; // Reset for new session
    std::cout << "[FairPlay] fp-setup phase 1: mode=" << mode << " → 142 bytes\n";
    return response;
}

std::vector<uint8_t> FairPlay::handshake(const uint8_t* req, int len) {
    if (len < 164) {
        std::cerr << "[FairPlay] handshake: invalid request length " << len << "\n";
        return {};
    }

    if (req[4] != 0x03) {
        std::cerr << "[FairPlay] handshake: unsupported version " << (int)req[4] << "\n";
        return {};
    }

    // Store the message for later fairplay_decrypt
    memcpy(keymsg_, req, 164);
    keymsg_len_ = 164;

    // Response: fp_header (12 bytes) + last 20 bytes from request
    std::vector<uint8_t> response(32);
    memcpy(response.data(), fp_header, 12);
    memcpy(response.data() + 12, req + 144, 20);

    std::cout << "[FairPlay] fp-setup phase 2: stored 164-byte key message → 32 bytes\n";
    return response;
}

bool FairPlay::decrypt(const uint8_t* input, uint8_t* output) {
    if (keymsg_len_ != 164) {
        return false;
    }

    // Debug: dump keymsg header
    std::cout << "[FairPlay] keymsg[0:15]: ";
    for (int i = 0; i < 16; i++)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)keymsg_[i];
    std::cout << "\n[FairPlay] keymsg mode byte [12]: " << std::hex
              << std::setfill('0') << std::setw(2) << (int)keymsg_[12] << std::dec << "\n";

    playfair_decrypt(keymsg_, const_cast<uint8_t*>(input), output);
    std::cout << "[FairPlay] decrypt: AES key decrypted via PlayFair\n";
    return true;
}

} // namespace openmirror::airplay
