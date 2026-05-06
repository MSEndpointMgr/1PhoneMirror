#include <openmirror/airplay/srp_pin.h>

#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

#include <cstring>
#include <iostream>

namespace openmirror::airplay {

namespace {

// RFC 5054 Group 14 — 2048-bit safe prime, generator g = 2.
// This is the modulus AirPlay 1 / classic Apple TV uses for PIN pairing.
constexpr const char* kRFC5054_Group14_N_hex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
    "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
    "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AACAA68FFFFFFFFFFFFFFFF";

constexpr int kModBytes = 256;        // 2048 bits / 8
constexpr int kHashBytes = SHA_DIGEST_LENGTH; // 20

// SHA-1 helper.
std::vector<uint8_t> sha1(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out(kHashBytes);
    SHA1(in.data(), in.size(), out.data());
    return out;
}

// Append BIGNUM as big-endian, zero-padded to kModBytes.
void append_pad(std::vector<uint8_t>& dst, const BIGNUM* bn) {
    std::vector<uint8_t> tmp(kModBytes, 0);
    int n = BN_num_bytes(bn);
    if (n > kModBytes) n = kModBytes;
    BN_bn2bin(bn, tmp.data() + (kModBytes - n));
    dst.insert(dst.end(), tmp.begin(), tmp.end());
}

std::vector<uint8_t> bn_to_padded(const BIGNUM* bn) {
    std::vector<uint8_t> out(kModBytes, 0);
    int n = BN_num_bytes(bn);
    if (n > kModBytes) n = kModBytes;
    BN_bn2bin(bn, out.data() + (kModBytes - n));
    return out;
}

// SRP-6a "interleaved SHA-1" — strips leading zero bytes of S, splits
// the remaining bytes into even/odd, hashes each half with SHA-1, then
// interleaves the two 20-byte digests into a 40-byte key.
std::vector<uint8_t> sha1_interleave(const std::vector<uint8_t>& S) {
    // Strip leading zeros.
    size_t off = 0;
    while (off < S.size() && S[off] == 0) ++off;
    size_t n = S.size() - off;
    if (n & 1) { ++off; --n; } // also drop one more if odd length

    std::vector<uint8_t> e(n / 2);
    std::vector<uint8_t> o(n / 2);
    for (size_t i = 0; i < n / 2; i++) {
        e[i] = S[off + 2 * i];
        o[i] = S[off + 2 * i + 1];
    }

    uint8_t he[kHashBytes], ho[kHashBytes];
    SHA1(e.data(), e.size(), he);
    SHA1(o.data(), o.size(), ho);

    std::vector<uint8_t> K(2 * kHashBytes);
    for (int i = 0; i < kHashBytes; i++) {
        K[2 * i]     = he[i];
        K[2 * i + 1] = ho[i];
    }
    return K;
}

} // namespace

struct SrpPinServer::Impl {
    BN_CTX* ctx = nullptr;
    BIGNUM* N = nullptr;
    BIGNUM* g = nullptr;
    BIGNUM* k = nullptr;
    BIGNUM* s = nullptr;   // salt (16 bytes)
    BIGNUM* v = nullptr;   // verifier
    BIGNUM* b = nullptr;   // server private
    BIGNUM* B = nullptr;   // server public
    BIGNUM* A = nullptr;   // client public
    std::vector<uint8_t> M2;
    std::vector<uint8_t> K;     // 40-byte interleaved session key
    std::string username = "Pair-Setup";
    bool ready = false;

    Impl() {
        ctx = BN_CTX_new();
        N = BN_new();
        g = BN_new();
        k = BN_new();
        s = BN_new();
        v = BN_new();
        b = BN_new();
        B = BN_new();
        A = BN_new();
    }
    ~Impl() {
        BN_free(N); BN_free(g); BN_free(k); BN_free(s);
        BN_free(v); BN_free(b); BN_free(B); BN_free(A);
        BN_CTX_free(ctx);
    }
};

SrpPinServer::SrpPinServer() : impl_(new Impl()) {}
SrpPinServer::~SrpPinServer() { delete impl_; }

bool SrpPinServer::start(const std::string& pin, const std::string& username) {
    authenticated_ = false;
    impl_->ready = false;
    impl_->M2.clear();
    impl_->K.clear();
    impl_->username = username.empty() ? "Pair-Setup" : username;

    if (!BN_hex2bn(&impl_->N, kRFC5054_Group14_N_hex)) return false;
    if (!BN_set_word(impl_->g, 2)) return false;

    // k = SHA1(N | PAD(g))
    std::vector<uint8_t> kbuf;
    append_pad(kbuf, impl_->N);
    append_pad(kbuf, impl_->g);
    auto kdig = sha1(kbuf);
    if (!BN_bin2bn(kdig.data(), (int)kdig.size(), impl_->k)) return false;

    // salt s — 16 random bytes
    uint8_t salt_bytes[16];
    if (RAND_bytes(salt_bytes, sizeof(salt_bytes)) != 1) return false;
    if (!BN_bin2bn(salt_bytes, sizeof(salt_bytes), impl_->s)) return false;

    // x = SHA1(s | SHA1(I | ":" | P))
    std::vector<uint8_t> ip;
    ip.insert(ip.end(), impl_->username.begin(), impl_->username.end());
    ip.push_back(':');
    ip.insert(ip.end(), pin.begin(), pin.end());
    auto ip_h = sha1(ip);

    std::vector<uint8_t> sx(salt_bytes, salt_bytes + sizeof(salt_bytes));
    sx.insert(sx.end(), ip_h.begin(), ip_h.end());
    auto x_h = sha1(sx);

    BIGNUM* x = BN_new();
    if (!BN_bin2bn(x_h.data(), (int)x_h.size(), x)) { BN_free(x); return false; }

    // v = g^x mod N
    if (!BN_mod_exp(impl_->v, impl_->g, x, impl_->N, impl_->ctx)) {
        BN_free(x); return false;
    }
    BN_free(x);

    // b = random (256 bytes), B = (k*v + g^b) mod N
    if (!BN_rand(impl_->b, kModBytes * 8 - 1, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY)) {
        return false;
    }
    BIGNUM* gb = BN_new();
    BIGNUM* kv = BN_new();
    BIGNUM* sum = BN_new();
    bool ok = BN_mod_exp(gb, impl_->g, impl_->b, impl_->N, impl_->ctx) &&
              BN_mod_mul(kv, impl_->k, impl_->v, impl_->N, impl_->ctx) &&
              BN_mod_add(sum, kv, gb, impl_->N, impl_->ctx) &&
              BN_copy(impl_->B, sum) != nullptr;
    BN_free(gb); BN_free(kv); BN_free(sum);
    if (!ok) return false;

    impl_->ready = true;
    return true;
}

std::vector<uint8_t> SrpPinServer::get_salt() const {
    if (!impl_->ready) return {};
    std::vector<uint8_t> out(16, 0);
    int n = BN_num_bytes(impl_->s);
    if (n > 16) n = 16;
    BN_bn2bin(impl_->s, out.data() + (16 - n));
    return out;
}

std::vector<uint8_t> SrpPinServer::get_B() const {
    if (!impl_->ready) return {};
    return bn_to_padded(impl_->B);
}

bool SrpPinServer::process_client_pubkey(const std::vector<uint8_t>& A,
                                         const std::vector<uint8_t>& M1) {
    if (!impl_->ready || A.size() != kModBytes || M1.size() != kHashBytes)
        return false;

    if (!BN_bin2bn(A.data(), (int)A.size(), impl_->A)) return false;

    // Reject A ≡ 0 (mod N).
    BIGNUM* tmp = BN_new();
    BN_mod(tmp, impl_->A, impl_->N, impl_->ctx);
    if (BN_is_zero(tmp)) { BN_free(tmp); return false; }
    BN_free(tmp);

    // u = SHA1(PAD(A) | PAD(B))
    std::vector<uint8_t> ub;
    append_pad(ub, impl_->A);
    append_pad(ub, impl_->B);
    auto uh = sha1(ub);
    BIGNUM* u = BN_new();
    if (!BN_bin2bn(uh.data(), (int)uh.size(), u)) { BN_free(u); return false; }

    // S = (A * v^u)^b mod N
    BIGNUM* vu = BN_new();
    BIGNUM* avu = BN_new();
    BIGNUM* S = BN_new();
    bool ok = BN_mod_exp(vu, impl_->v, u, impl_->N, impl_->ctx) &&
              BN_mod_mul(avu, impl_->A, vu, impl_->N, impl_->ctx) &&
              BN_mod_exp(S, avu, impl_->b, impl_->N, impl_->ctx);
    BN_free(vu); BN_free(avu); BN_free(u);
    if (!ok) { BN_free(S); return false; }

    auto S_bytes = bn_to_padded(S);
    BN_free(S);

    // K = SHA1_INTERLEAVE(S)
    impl_->K = sha1_interleave(S_bytes);

    // M1' = SHA1( SHA1(N) XOR SHA1(g) | SHA1(I) | s | A | B | K )
    auto hN = sha1(bn_to_padded(impl_->N));
    std::vector<uint8_t> g_padded(kModBytes, 0);
    {
        int gn = BN_num_bytes(impl_->g);
        BN_bn2bin(impl_->g, g_padded.data() + (kModBytes - gn));
    }
    auto hg = sha1(g_padded);
    std::vector<uint8_t> hNg(kHashBytes);
    for (int i = 0; i < kHashBytes; i++) hNg[i] = hN[i] ^ hg[i];

    std::vector<uint8_t> hI_in(impl_->username.begin(), impl_->username.end());
    auto hI = sha1(hI_in);

    std::vector<uint8_t> m1_buf;
    m1_buf.insert(m1_buf.end(), hNg.begin(), hNg.end());
    m1_buf.insert(m1_buf.end(), hI.begin(), hI.end());
    m1_buf.insert(m1_buf.end(), get_salt().begin(), get_salt().end());
    append_pad(m1_buf, impl_->A);
    append_pad(m1_buf, impl_->B);
    m1_buf.insert(m1_buf.end(), impl_->K.begin(), impl_->K.end());
    auto M1_calc = sha1(m1_buf);

    if (CRYPTO_memcmp(M1_calc.data(), M1.data(), kHashBytes) != 0) {
        std::cerr << "[SRP] Client M1 proof mismatch (wrong PIN?)\n";
        return false;
    }

    // M2 = SHA1(A | M1 | K)
    std::vector<uint8_t> m2_buf;
    append_pad(m2_buf, impl_->A);
    m2_buf.insert(m2_buf.end(), M1.begin(), M1.end());
    m2_buf.insert(m2_buf.end(), impl_->K.begin(), impl_->K.end());
    impl_->M2 = sha1(m2_buf);

    authenticated_ = true;
    return true;
}

std::vector<uint8_t> SrpPinServer::get_M2() const {
    return impl_->M2;
}

std::vector<uint8_t> SrpPinServer::get_session_key() const {
    return impl_->K;
}

// ---- Step-3 LTPK encryption/decryption ----
//
// AES-128-CBC with PKCS#7 padding.
// AES key = SHA1("Pair-Setup-AES-Key"  | K)[0..16]
// AES IV  = SHA1("Pair-Setup-AES-IV"   | K)[0..16]
// Same key/IV is used for both directions on AirPlay 1.

namespace {

bool derive_aes(const std::vector<uint8_t>& K,
                uint8_t out_key[16], uint8_t out_iv[16]) {
    if (K.empty()) return false;

    auto h = [&](const char* tag, uint8_t out[16]) {
        std::vector<uint8_t> buf(tag, tag + std::strlen(tag));
        buf.insert(buf.end(), K.begin(), K.end());
        uint8_t dig[SHA_DIGEST_LENGTH];
        SHA1(buf.data(), buf.size(), dig);
        std::memcpy(out, dig, 16);
    };
    h("Pair-Setup-AES-Key", out_key);
    h("Pair-Setup-AES-IV",  out_iv);
    return true;
}

} // namespace

std::vector<uint8_t> srp_pin_decrypt_ltpk(const std::vector<uint8_t>& K,
                                          const std::vector<uint8_t>& epk) {
    uint8_t key[16], iv[16];
    if (!derive_aes(K, key, iv) || epk.empty()) return {};

    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return {};
    std::vector<uint8_t> out(epk.size() + 16);
    int outl = 0, finl = 0;
    bool ok = EVP_DecryptInit_ex(c, EVP_aes_128_cbc(), nullptr, key, iv) == 1 &&
              EVP_DecryptUpdate(c, out.data(), &outl, epk.data(), (int)epk.size()) == 1 &&
              EVP_DecryptFinal_ex(c, out.data() + outl, &finl) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    out.resize(outl + finl);
    return out;
}

std::vector<uint8_t> srp_pin_encrypt_ltpk(const std::vector<uint8_t>& K,
                                          const std::vector<uint8_t>& our_ltpk) {
    uint8_t key[16], iv[16];
    if (!derive_aes(K, key, iv) || our_ltpk.empty()) return {};

    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return {};
    std::vector<uint8_t> out(our_ltpk.size() + 32);
    int outl = 0, finl = 0;
    bool ok = EVP_EncryptInit_ex(c, EVP_aes_128_cbc(), nullptr, key, iv) == 1 &&
              EVP_EncryptUpdate(c, out.data(), &outl, our_ltpk.data(), (int)our_ltpk.size()) == 1 &&
              EVP_EncryptFinal_ex(c, out.data() + outl, &finl) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    out.resize(outl + finl);
    return out;
}

} // namespace openmirror::airplay
