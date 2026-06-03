#include <opm/airplay/hap_srp.h>

#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#include <cstring>
#include <iostream>
#include <memory>

namespace opm::airplay {

namespace {

// ----------------------------------------------------------------------------
// RFC 3526 group 15 — 3072-bit MODP prime, g = 5
// ----------------------------------------------------------------------------

static const char* kN3072_hex =
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
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64"
    "ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B"
    "F12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31"
    "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

constexpr int kPrimeLen = 384; // 3072 / 8

struct BnDeleter { void operator()(BIGNUM* p) const { if (p) BN_free(p); } };
struct CtxDeleter { void operator()(BN_CTX* p) const { if (p) BN_CTX_free(p); } };
using BnUp = std::unique_ptr<BIGNUM, BnDeleter>;
using CtxUp = std::unique_ptr<BN_CTX, CtxDeleter>;

BnUp new_bn() { return BnUp(BN_new()); }
BnUp bn_from_hex(const char* hex) {
    BIGNUM* p = nullptr;
    BN_hex2bn(&p, hex);
    return BnUp(p);
}
BnUp bn_from_bytes(const uint8_t* p, size_t n) {
    BIGNUM* b = BN_new();
    BN_bin2bn(p, static_cast<int>(n), b);
    return BnUp(b);
}

// Pad to kPrimeLen with leading zeros (PAD() in SRP-6a).
std::vector<uint8_t> bn_to_padded(const BIGNUM* b) {
    std::vector<uint8_t> out(kPrimeLen, 0);
    int n = BN_num_bytes(b);
    if (n > kPrimeLen) return out; // shouldn't happen for our moduli
    BN_bn2bin(b, out.data() + (kPrimeLen - n));
    return out;
}

std::vector<uint8_t> sha512(const uint8_t* d, size_t n) {
    std::vector<uint8_t> out(SHA512_DIGEST_LENGTH);
    SHA512(d, n, out.data());
    return out;
}

std::vector<uint8_t> sha512_concat(std::initializer_list<std::pair<const uint8_t*, size_t>> parts) {
    SHA512_CTX c; SHA512_Init(&c);
    for (auto& p : parts) SHA512_Update(&c, p.first, p.second);
    std::vector<uint8_t> out(SHA512_DIGEST_LENGTH);
    SHA512_Final(out.data(), &c);
    return out;
}

// k = H(N | PAD(g))  — SRP-6a standard, padded g to N's length.
std::vector<uint8_t> compute_k(const BIGNUM* N, const BIGNUM* g) {
    auto Nb = bn_to_padded(N);
    auto gb = bn_to_padded(g);
    return sha512_concat({{Nb.data(), Nb.size()}, {gb.data(), gb.size()}});
}

// x = H(salt | H(I | ":" | p))
BnUp compute_x(const std::string& I, const std::string& p,
               const std::vector<uint8_t>& salt) {
    // inner = H(I | ":" | p)
    std::string ip = I + ":" + p;
    auto inner = sha512(reinterpret_cast<const uint8_t*>(ip.data()), ip.size());
    auto outer = sha512_concat({{salt.data(), salt.size()},
                                {inner.data(), inner.size()}});
    return bn_from_bytes(outer.data(), outer.size());
}

// u = H(PAD(A) | PAD(B))
BnUp compute_u(const BIGNUM* A, const BIGNUM* B) {
    auto Ap = bn_to_padded(A);
    auto Bp = bn_to_padded(B);
    auto h = sha512_concat({{Ap.data(), Ap.size()}, {Bp.data(), Bp.size()}});
    return bn_from_bytes(h.data(), h.size());
}

// M1 = H( H(N) XOR H(g) | H(I) | salt | A | B | K )
std::vector<uint8_t> compute_M1(const BIGNUM* N, const BIGNUM* g,
                                 const std::string& I,
                                 const std::vector<uint8_t>& salt,
                                 const BIGNUM* A, const BIGNUM* B,
                                 const std::vector<uint8_t>& K) {
    auto Nb = bn_to_padded(N);
    auto gb = bn_to_padded(g);
    auto hN = sha512(Nb.data(), Nb.size());
    auto hg = sha512(gb.data(), gb.size());
    std::vector<uint8_t> hN_xor_hg(hN.size());
    for (size_t i = 0; i < hN.size(); ++i) hN_xor_hg[i] = hN[i] ^ hg[i];
    auto hI = sha512(reinterpret_cast<const uint8_t*>(I.data()), I.size());
    auto Ap = bn_to_padded(A);
    auto Bp = bn_to_padded(B);
    return sha512_concat({
        {hN_xor_hg.data(), hN_xor_hg.size()},
        {hI.data(), hI.size()},
        {salt.data(), salt.size()},
        {Ap.data(), Ap.size()},
        {Bp.data(), Bp.size()},
        {K.data(), K.size()},
    });
}

// M2 = H( A | M1 | K )
std::vector<uint8_t> compute_M2(const BIGNUM* A,
                                 const std::vector<uint8_t>& M1,
                                 const std::vector<uint8_t>& K) {
    auto Ap = bn_to_padded(A);
    return sha512_concat({
        {Ap.data(), Ap.size()},
        {M1.data(), M1.size()},
        {K.data(), K.size()},
    });
}

} // namespace

// ----------------------------------------------------------------------------
// HapSrpServer
// ----------------------------------------------------------------------------

struct HapSrpServer::Impl {
    BnUp N;
    BnUp g;
    BnUp b;            // server private
    BnUp B;            // server public
    BnUp v;            // verifier
    std::vector<uint8_t> salt;
    std::string I = "Pair-Setup";
    bool started = false;
};

HapSrpServer::HapSrpServer() : impl_(new Impl) {
    impl_->N = bn_from_hex(kN3072_hex);
    impl_->g = new_bn(); BN_set_word(impl_->g.get(), 5);
}

HapSrpServer::~HapSrpServer() { delete impl_; }

bool HapSrpServer::start(const std::string& setup_code,
                          std::vector<uint8_t>& out_salt,
                          std::vector<uint8_t>& out_B) {
    CtxUp ctx(BN_CTX_new());
    if (!ctx) return false;

    impl_->salt.assign(16, 0);
    if (RAND_bytes(impl_->salt.data(), 16) != 1) return false;

    // v = g^x mod N
    BnUp x = compute_x(impl_->I, setup_code, impl_->salt);
    impl_->v = new_bn();
    if (BN_mod_exp(impl_->v.get(), impl_->g.get(), x.get(),
                   impl_->N.get(), ctx.get()) != 1) return false;

    // b in [1, N-1], 256 bits is fine — RFC 5054 says "at least 256 bits".
    impl_->b = new_bn();
    if (BN_rand(impl_->b.get(), 256, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1)
        return false;

    // k = H(N | PAD(g))
    auto kbytes = compute_k(impl_->N.get(), impl_->g.get());
    BnUp k = bn_from_bytes(kbytes.data(), kbytes.size());

    // B = (k*v + g^b) mod N
    BnUp kv(BN_new()), gb(BN_new()), sum(BN_new());
    if (BN_mod_mul(kv.get(), k.get(), impl_->v.get(),
                   impl_->N.get(), ctx.get()) != 1) return false;
    if (BN_mod_exp(gb.get(), impl_->g.get(), impl_->b.get(),
                   impl_->N.get(), ctx.get()) != 1) return false;
    if (BN_mod_add(sum.get(), kv.get(), gb.get(),
                   impl_->N.get(), ctx.get()) != 1) return false;
    impl_->B = std::move(sum);

    out_salt = impl_->salt;
    out_B = bn_to_padded(impl_->B.get());
    impl_->started = true;
    return true;
}

std::vector<uint8_t> HapSrpServer::verify_client_proof(
    const std::vector<uint8_t>& A_bytes,
    const std::vector<uint8_t>& M1_client) {

    if (!impl_->started || A_bytes.size() != kPrimeLen ||
        M1_client.size() != SHA512_DIGEST_LENGTH) {
        return {};
    }
    CtxUp ctx(BN_CTX_new());
    if (!ctx) return {};

    BnUp A = bn_from_bytes(A_bytes.data(), A_bytes.size());

    // A mod N == 0  →  abort.
    BnUp tmp(BN_new());
    if (BN_mod(tmp.get(), A.get(), impl_->N.get(), ctx.get()) != 1) return {};
    if (BN_is_zero(tmp.get())) return {};

    // u = H(PAD(A) | PAD(B))
    BnUp u = compute_u(A.get(), impl_->B.get());
    if (BN_is_zero(u.get())) return {};

    // S = (A * v^u)^b mod N
    BnUp vu(BN_new()), avu(BN_new()), S(BN_new());
    if (BN_mod_exp(vu.get(), impl_->v.get(), u.get(),
                   impl_->N.get(), ctx.get()) != 1) return {};
    if (BN_mod_mul(avu.get(), A.get(), vu.get(),
                   impl_->N.get(), ctx.get()) != 1) return {};
    if (BN_mod_exp(S.get(), avu.get(), impl_->b.get(),
                   impl_->N.get(), ctx.get()) != 1) return {};

    auto Sb = bn_to_padded(S.get());
    K_ = sha512(Sb.data(), Sb.size());

    auto M1_server = compute_M1(impl_->N.get(), impl_->g.get(), impl_->I,
                                 impl_->salt, A.get(), impl_->B.get(), K_);
    if (M1_server.size() != M1_client.size() ||
        CRYPTO_memcmp(M1_server.data(), M1_client.data(), M1_server.size()) != 0) {
        K_.clear();
        return {};
    }
    return compute_M2(A.get(), M1_client, K_);
}

// ----------------------------------------------------------------------------
// Self-test — full client+server round trip in-process.
// ----------------------------------------------------------------------------

bool hap_srp_self_test() {
    using std::cerr;

    HapSrpServer srv;
    std::vector<uint8_t> salt, B;
    const std::string pwd = "031-45-154";
    if (!srv.start(pwd, salt, B) || salt.size() != 16 || B.size() != kPrimeLen) {
        cerr << "[HAP-SRP] server start failed\n"; return false;
    }

    // ---- client side ----
    CtxUp ctx(BN_CTX_new());
    BnUp N = bn_from_hex(kN3072_hex);
    BnUp g = new_bn(); BN_set_word(g.get(), 5);

    // a random, A = g^a mod N
    BnUp a = new_bn();
    if (BN_rand(a.get(), 256, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1) {
        cerr << "[HAP-SRP] client a rand failed\n"; return false;
    }
    BnUp A = new_bn();
    if (BN_mod_exp(A.get(), g.get(), a.get(), N.get(), ctx.get()) != 1) {
        cerr << "[HAP-SRP] client A failed\n"; return false;
    }
    auto Ab = bn_to_padded(A.get());

    // x = H(salt | H(I:p))
    const std::string I = "Pair-Setup";
    BnUp x = compute_x(I, pwd, salt);

    // u = H(PAD(A) | PAD(B))
    BnUp Bb = bn_from_bytes(B.data(), B.size());
    BnUp u = compute_u(A.get(), Bb.get());

    // k = H(N | PAD(g))
    auto kbytes = compute_k(N.get(), g.get());
    BnUp k = bn_from_bytes(kbytes.data(), kbytes.size());

    // S = (B - k*g^x) ^ (a + u*x) mod N
    BnUp gx(BN_new()), kgx(BN_new()), base(BN_new());
    BN_mod_exp(gx.get(), g.get(), x.get(), N.get(), ctx.get());
    BN_mod_mul(kgx.get(), k.get(), gx.get(), N.get(), ctx.get());
    BN_mod_sub(base.get(), Bb.get(), kgx.get(), N.get(), ctx.get());

    BnUp ux(BN_new()), exp(BN_new()), S(BN_new());
    BN_mul(ux.get(), u.get(), x.get(), ctx.get());
    BN_add(exp.get(), a.get(), ux.get());
    BN_mod_exp(S.get(), base.get(), exp.get(), N.get(), ctx.get());

    auto Sb = bn_to_padded(S.get());
    auto K = sha512(Sb.data(), Sb.size());

    // M1 = H(H(N) XOR H(g) | H(I) | s | A | B | K)
    auto M1 = compute_M1(N.get(), g.get(), I, salt, A.get(), Bb.get(), K);

    auto M2 = srv.verify_client_proof(Ab, M1);
    if (M2.empty()) {
        cerr << "[HAP-SRP] server rejected client M1\n"; return false;
    }
    // Client verifies M2 against H(A | M1 | K).
    auto M2_expect = compute_M2(A.get(), M1, K);
    if (M2 != M2_expect) {
        cerr << "[HAP-SRP] M2 mismatch (client view)\n"; return false;
    }
    if (srv.session_key() != K) {
        cerr << "[HAP-SRP] session-key mismatch\n"; return false;
    }

    // Wrong password must be rejected.
    {
        HapSrpServer srv2;
        std::vector<uint8_t> s2, B2;
        srv2.start("000-00-000", s2, B2);
        auto M2_bad = srv2.verify_client_proof(Ab, M1);
        if (!M2_bad.empty()) {
            cerr << "[HAP-SRP] wrong-password did NOT fail\n"; return false;
        }
    }

    cerr << "[HAP-SRP] PASS (K = SHA-512(S), 64 bytes, client+server match)\n";
    return true;
}

} // namespace opm::airplay
