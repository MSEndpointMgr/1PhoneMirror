#include <opm/airplay/mirror_buffer.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <cstring>
#include <cinttypes>
#include <cstdio>
#include <iostream>
#include <iomanip>

namespace opm::airplay {

MirrorBuffer::MirrorBuffer() = default;

MirrorBuffer::~MirrorBuffer() {
    if (ctx_) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

void MirrorBuffer::set_aes_key(const uint8_t key[16]) {
    memcpy(aes_key_, key, 16);
    std::cout << "[MirrorBuffer] AES key set: ";
    for (int i = 0; i < 16; i++)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)key[i];
    std::cout << std::dec << "\n";
}

void MirrorBuffer::init_aes(uint64_t stream_connection_id) {
    char key_str[64] = {};
    char iv_str[64] = {};
    snprintf(key_str, sizeof(key_str), "AirPlayStreamKey%" PRIu64, stream_connection_id);
    snprintf(iv_str, sizeof(iv_str), "AirPlayStreamIV%" PRIu64, stream_connection_id);

    unsigned char key_hash[SHA512_DIGEST_LENGTH];
    unsigned char iv_hash[SHA512_DIGEST_LENGTH];

    // Key = SHA-512(key_string + aeskey_audio)
    {
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md, EVP_sha512(), nullptr);
        EVP_DigestUpdate(md, key_str, strlen(key_str));
        EVP_DigestUpdate(md, aes_key_, 16);
        EVP_DigestFinal_ex(md, key_hash, nullptr);
        EVP_MD_CTX_free(md);
    }

    // IV = SHA-512(iv_string + aeskey_audio)
    {
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md, EVP_sha512(), nullptr);
        EVP_DigestUpdate(md, iv_str, strlen(iv_str));
        EVP_DigestUpdate(md, aes_key_, 16);
        EVP_DigestFinal_ex(md, iv_hash, nullptr);
        EVP_MD_CTX_free(md);
    }

    // Use first 16 bytes of each hash
    if (ctx_) {
        EVP_CIPHER_CTX_free(ctx_);
    }
    ctx_ = EVP_CIPHER_CTX_new();
    // CTR mode: encrypt == decrypt. UxPlay uses encrypt direction.
    EVP_EncryptInit_ex(ctx_, EVP_aes_128_ctr(), nullptr, key_hash, iv_hash);
    EVP_CIPHER_CTX_set_padding(ctx_, 0);

    std::cout << "[MirrorBuffer] AES-CTR initialized for connID=" << stream_connection_id << "\n";
    std::cout << "[MirrorBuffer] key_str: \"" << key_str << "\" (len=" << strlen(key_str) << ")\n";
    std::cout << "[MirrorBuffer] iv_str:  \"" << iv_str << "\" (len=" << strlen(iv_str) << ")\n";
    std::cout << "[MirrorBuffer] aes_key: ";
    for (int i = 0; i < 16; i++)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)aes_key_[i];
    std::cout << "\n[MirrorBuffer] SHA512(key_str+aes_key) first 16: ";
    for (int i = 0; i < 16; i++)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)key_hash[i];
    std::cout << "\n[MirrorBuffer] SHA512(iv_str+aes_key)  first 16: ";
    for (int i = 0; i < 16; i++)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)iv_hash[i];

    // Self-test: generate first 16 keystream bytes
    {
        EVP_CIPHER_CTX* test_ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(test_ctx, EVP_aes_128_ctr(), nullptr, key_hash, iv_hash);
        EVP_CIPHER_CTX_set_padding(test_ctx, 0);
        uint8_t zeros[16] = {0};
        uint8_t keystream[16] = {0};
        int out_len = 0;
        EVP_EncryptUpdate(test_ctx, keystream, &out_len, zeros, 16);
        EVP_CIPHER_CTX_free(test_ctx);
        std::cout << "\n[MirrorBuffer] first 16 keystream bytes: ";
        for (int i = 0; i < 16; i++)
            std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)keystream[i];
    }
    std::cout << std::dec << "\n";
}

void MirrorBuffer::decrypt(uint8_t* input, uint8_t* output, int len) {
    if (!ctx_) return;
    // CTR mode: encrypt == decrypt. Use EncryptUpdate to match UxPlay.
    int out_len = 0;
    EVP_EncryptUpdate(ctx_, output, &out_len, input, len);
}

} // namespace opm::airplay
