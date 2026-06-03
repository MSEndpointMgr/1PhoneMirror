#include <opm/airplay/tlv8.h>

#include <cstring>
#include <iostream>

namespace opm::airplay {

void TlvWriter::add(TlvType tag, const uint8_t* data, size_t len) {
    const uint8_t t = static_cast<uint8_t>(tag);
    if (len == 0) {
        buf_.push_back(t);
        buf_.push_back(0);
        return;
    }
    size_t off = 0;
    while (off < len) {
        const size_t chunk = std::min<size_t>(255, len - off);
        buf_.push_back(t);
        buf_.push_back(static_cast<uint8_t>(chunk));
        buf_.insert(buf_.end(), data + off, data + off + chunk);
        off += chunk;
    }
}

bool TlvReader::parse(const uint8_t* data, size_t len) {
    items_.clear();
    items_raw_.clear();
    size_t i = 0;
    uint8_t prev_tag = 0xAA;        // sentinel that can't collide on item 0
    bool have_prev = false;
    while (i < len) {
        if (i + 2 > len) return false;
        uint8_t tag = data[i++];
        uint8_t l = data[i++];
        if (i + l > len) return false;

        items_raw_.emplace_back(tag, std::vector<uint8_t>(data + i, data + i + l));

        // Fragment join: consecutive 255-byte items with same tag belong to
        // one logical value. Separator (0xFF) breaks the run.
        if (have_prev && tag == prev_tag && tag != (uint8_t)TlvType::Separator) {
            auto& acc = items_[tag];
            acc.insert(acc.end(), data + i, data + i + l);
        } else {
            // Start (or restart after a Separator) of a value.
            // Note: a later "group" (after Separator) will overwrite earlier
            // single-occurrence values in items_; callers that need
            // per-group access must use get_list().
            items_[tag].assign(data + i, data + i + l);
        }
        prev_tag = tag;
        have_prev = (l == 255);     // only continue-fragment runs use full 255B
        i += l;
    }
    return true;
}

std::optional<std::vector<uint8_t>> TlvReader::get(TlvType tag) const {
    auto it = items_.find(static_cast<uint8_t>(tag));
    if (it == items_.end()) return std::nullopt;
    return it->second;
}

std::optional<uint8_t> TlvReader::get_u8(TlvType tag) const {
    auto v = get(tag);
    if (!v || v->size() != 1) return std::nullopt;
    return (*v)[0];
}

std::vector<std::vector<uint8_t>> TlvReader::get_list(TlvType tag) const {
    std::vector<std::vector<uint8_t>> out;
    const uint8_t want = static_cast<uint8_t>(tag);
    const uint8_t sep  = static_cast<uint8_t>(TlvType::Separator);
    std::vector<uint8_t> acc;
    bool collecting = false;
    uint8_t prev = 0xAA;
    bool have_prev = false;

    auto flush = [&]() {
        if (collecting) {
            out.push_back(std::move(acc));
            acc.clear();
            collecting = false;
        }
    };

    for (auto& [t, v] : items_raw_) {
        if (t == sep) {
            flush();
            have_prev = false;
            continue;
        }
        if (t == want) {
            // Fragment join across same-tag 255-byte runs.
            if (collecting && have_prev && prev == want) {
                acc.insert(acc.end(), v.begin(), v.end());
            } else {
                flush();
                acc = v;
                collecting = true;
            }
            prev = t;
            have_prev = (v.size() == 255);
        } else {
            flush();
            prev = t;
            have_prev = false;
        }
    }
    flush();
    return out;
}

bool tlv8_self_test() {
    using std::cerr;

    // Round-trip a typical pair-setup M2 body: State=2, PublicKey(384B SRP B,
    // forces fragmentation across 2 items), Salt(16B).
    std::vector<uint8_t> pk(384);
    for (size_t i = 0; i < pk.size(); ++i) pk[i] = static_cast<uint8_t>(i * 7 + 3);
    std::vector<uint8_t> salt(16, 0xAB);

    TlvWriter w;
    w.add_u8(TlvType::State, 2);
    w.add(TlvType::PublicKey, pk);
    w.add(TlvType::Salt, salt);

    // Fragmentation: 384B PK should produce items of sizes 255 + 129 = 2 items.
    // Total wire size = 2 + (2 + 255) + (2 + 129) + 2+16 = 408.
    const auto wire = w.take();
    if (wire.size() != 1+1 + 1+1+255 + 1+1+129 + 1+1+16) {
        cerr << "[TLV8-TEST] unexpected wire size: " << wire.size() << "\n";
        return false;
    }

    TlvReader r;
    if (!r.parse(wire)) { cerr << "[TLV8-TEST] parse failed\n"; return false; }
    auto st = r.get_u8(TlvType::State);
    if (!st || *st != 2) { cerr << "[TLV8-TEST] state mismatch\n"; return false; }
    auto pk_r = r.get(TlvType::PublicKey);
    if (!pk_r || *pk_r != pk) {
        cerr << "[TLV8-TEST] public key roundtrip failed (got "
             << (pk_r ? pk_r->size() : 0) << "B, want " << pk.size() << "B)\n";
        return false;
    }
    auto salt_r = r.get(TlvType::Salt);
    if (!salt_r || *salt_r != salt) { cerr << "[TLV8-TEST] salt mismatch\n"; return false; }

    // Truncation: drop last byte → parse must fail cleanly.
    TlvReader r2;
    if (r2.parse(wire.data(), wire.size() - 1)) {
        cerr << "[TLV8-TEST] truncated buffer was accepted\n"; return false;
    }

    // Separator + list semantics (HAP list-pairings response shape):
    //   Identifier, PublicKey, Permissions, Separator, Identifier, PublicKey, Permissions
    TlvWriter w2;
    w2.add_string(TlvType::Identifier, "user-A");
    w2.add(TlvType::PublicKey, {1,2,3,4});
    w2.add_u8(TlvType::Permissions, 1);
    w2.add_separator();
    w2.add_string(TlvType::Identifier, "user-B");
    w2.add(TlvType::PublicKey, {5,6,7,8});
    w2.add_u8(TlvType::Permissions, 0);
    TlvReader r3;
    if (!r3.parse(w2.take())) { cerr << "[TLV8-TEST] list parse failed\n"; return false; }
    auto ids = r3.get_list(TlvType::Identifier);
    if (ids.size() != 2 || std::string(ids[0].begin(), ids[0].end()) != "user-A"
                       || std::string(ids[1].begin(), ids[1].end()) != "user-B") {
        cerr << "[TLV8-TEST] separator/list semantics broken (got " << ids.size() << " ids)\n";
        return false;
    }

    cerr << "[TLV8-TEST] PASS\n";
    return true;
}

} // namespace opm::airplay
