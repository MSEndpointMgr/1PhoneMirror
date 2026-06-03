#include <opm/airplay/hap_session.h>

#include <opm/airplay/hap_crypto.h>

#include <cstring>
#include <iostream>

namespace opm::airplay {

namespace {

void counter_to_nonce(uint64_t ctr, uint8_t nonce[12]) {
    std::memset(nonce, 0, 4);
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<uint8_t>((ctr >> (8 * i)) & 0xff);
    }
}

} // namespace

HapSession::HapSession(const std::vector<uint8_t>& read_key,
                       const std::vector<uint8_t>& write_key)
    : read_key_(read_key), write_key_(write_key) {}

std::vector<uint8_t> HapSession::encrypt_frame(const uint8_t* plain, size_t len) {
    if (!valid() || len == 0 || len > kMaxFrame) return {};

    uint8_t aad[2] = {
        static_cast<uint8_t>(len & 0xff),
        static_cast<uint8_t>((len >> 8) & 0xff),
    };
    uint8_t nonce[12];
    counter_to_nonce(write_counter_, nonce);

    auto ct = chacha20_poly1305_encrypt(write_key_.data(), nonce,
                                        aad, sizeof(aad),
                                        plain, len);
    if (ct.size() != len + 16) return {};

    std::vector<uint8_t> wire;
    wire.reserve(2 + ct.size());
    wire.push_back(aad[0]);
    wire.push_back(aad[1]);
    wire.insert(wire.end(), ct.begin(), ct.end());
    ++write_counter_;
    return wire;
}

std::vector<uint8_t> HapSession::decrypt_frame(const uint8_t* wire, size_t wire_len) {
    if (!valid() || wire_len < 2 + 16) return {};
    size_t plain_len = static_cast<size_t>(wire[0]) |
                       (static_cast<size_t>(wire[1]) << 8);
    if (plain_len == 0 || plain_len > kMaxFrame) return {};
    if (wire_len != 2 + plain_len + 16) return {};

    uint8_t aad[2] = { wire[0], wire[1] };
    uint8_t nonce[12];
    counter_to_nonce(read_counter_, nonce);

    auto pt = chacha20_poly1305_decrypt(read_key_.data(), nonce,
                                        aad, sizeof(aad),
                                        wire + 2, plain_len + 16);
    if (pt.size() != plain_len) return {};
    ++read_counter_;
    return pt;
}

bool hap_session_self_test() {
    // Two sessions wired back-to-back, like client+server. Server's write
    // key == client's read key (and vice versa). Counters MUST tick in
    // lock-step or AEAD auth fails on the next frame.
    std::vector<uint8_t> k1(32, 0xA5), k2(32, 0x5A);
    HapSession server(k2 /*read*/, k1 /*write*/);
    HapSession client(k1 /*read*/, k2 /*write*/);

    auto round_trip = [&](const std::vector<uint8_t>& msg) -> bool {
        auto wire = server.encrypt_frame(msg);
        if (wire.size() != 2 + msg.size() + 16) {
            std::cerr << "[HAP-session-test] bad wire len\n";
            return false;
        }
        auto got = client.decrypt_frame(wire);
        if (got != msg) {
            std::cerr << "[HAP-session-test] plaintext mismatch\n";
            return false;
        }
        return true;
    };

    // Multi-frame, both directions, including a max-size frame.
    for (size_t n : {1u, 5u, 64u, 1024u, 17u}) {
        std::vector<uint8_t> msg(n);
        for (size_t i = 0; i < n; ++i) msg[i] = static_cast<uint8_t>(i * 7 + 1);
        if (!round_trip(msg)) return false;
    }
    // Reverse direction
    {
        std::vector<uint8_t> msg{'p','i','n','g'};
        auto wire = client.encrypt_frame(msg);
        auto got = server.decrypt_frame(wire);
        if (got != msg) {
            std::cerr << "[HAP-session-test] reverse direction fail\n";
            return false;
        }
    }

    // Tag tamper must reject without bumping counter.
    {
        std::vector<uint8_t> msg{'x','y','z'};
        auto wire = server.encrypt_frame(msg);
        wire.back() ^= 0x01;
        uint64_t before = client.read_counter();
        auto got = client.decrypt_frame(wire);
        if (!got.empty()) {
            std::cerr << "[HAP-session-test] tampered frame accepted!\n";
            return false;
        }
        if (client.read_counter() != before) {
            std::cerr << "[HAP-session-test] counter advanced on auth fail\n";
            return false;
        }
        // Re-encrypt the original; counters must still align.
        // Server already incremented write_counter when it encrypted `wire`,
        // and client did NOT increment read_counter (tamper rejected).
        // Send a fresh frame with the original plaintext through.
        auto wire2 = server.encrypt_frame(msg);
        auto got2 = client.decrypt_frame(wire2);
        if (got2 != msg) {
            std::cerr << "[HAP-session-test] counter resync failed\n";
            return false;
        }
    }

    // Oversize plaintext rejected.
    {
        std::vector<uint8_t> too_big(1025, 0);
        if (!server.encrypt_frame(too_big).empty()) {
            std::cerr << "[HAP-session-test] oversize accepted\n";
            return false;
        }
    }

    std::cout << "[HAP-session-test] OK \xE2\x80\x94 frame AEAD + counters + tamper reject.\n";
    return true;
}

} // namespace opm::airplay
