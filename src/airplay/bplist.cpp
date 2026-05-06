#include <openmirror/airplay/bplist.h>
#include <cstring>
#include <iostream>

namespace openmirror::airplay {

// ===== BPlistReader =====

uint64_t BPlistReader::read_be(size_t offset, int bytes) const {
    if (offset + bytes > len_) return 0;
    uint64_t val = 0;
    for (int i = 0; i < bytes; i++) {
        val = (val << 8) | data_[offset + i];
    }
    return val;
}

uint64_t BPlistReader::get_offset(uint64_t obj_idx) const {
    if (obj_idx >= num_objects_) return 0;
    return offsets_[obj_idx];
}

std::pair<uint64_t, size_t> BPlistReader::decode_count(size_t offset) const {
    if (offset >= len_) return {0, 0};
    uint64_t count = data_[offset] & 0x0F;
    size_t extra = 0;

    if (count == 0x0F) {
        // Next byte is an int marker encoding the actual count
        if (offset + 2 >= len_) return {0, 0};
        uint8_t int_marker = data_[offset + 1];
        int int_bytes = 1 << (int_marker & 0x0F);
        if (offset + 2 + int_bytes > len_) return {0, 0};
        count = read_be(offset + 2, int_bytes);
        extra = 1 + int_bytes;
    }

    return {count, extra};
}

bool BPlistReader::parse(const uint8_t* data, size_t len) {
    if (len < 40) return false;
    if (memcmp(data, "bplist00", 8) != 0) return false;

    data_ = data;
    len_ = len;

    const uint8_t* trailer = data + len - 32;
    offset_size_ = trailer[6];
    ref_size_ = trailer[7];
    num_objects_ = read_be(len - 32 + 8, 8);
    root_object_ = read_be(len - 32 + 16, 8);
    offset_table_off_ = read_be(len - 32 + 24, 8);

    if (offset_size_ == 0 || offset_size_ > 8) return false;
    if (ref_size_ == 0 || ref_size_ > 8) return false;
    if (num_objects_ == 0 || num_objects_ > 100000) return false;
    if (root_object_ >= num_objects_) return false;
    if (offset_table_off_ + num_objects_ * offset_size_ > len - 32) return false;

    offsets_.resize(num_objects_);
    for (uint64_t i = 0; i < num_objects_; i++) {
        offsets_[i] = read_be(
            static_cast<size_t>(offset_table_off_ + i * offset_size_),
            offset_size_);
        if (offsets_[i] >= len) return false;
    }

    return true;
}

uint64_t BPlistReader::read_uint_object(uint64_t obj_idx) const {
    size_t off = static_cast<size_t>(get_offset(obj_idx));
    if (off >= len_) return 0;

    uint8_t marker = data_[off];
    if ((marker >> 4) != 0x1) return 0;

    int byte_count = 1 << (marker & 0x0F);
    if (off + 1 + byte_count > len_) return 0;
    return read_be(off + 1, byte_count);
}

std::string BPlistReader::read_string_object(uint64_t obj_idx) const {
    size_t off = static_cast<size_t>(get_offset(obj_idx));
    if (off >= len_) return "";

    uint8_t marker = data_[off];
    uint8_t type = marker >> 4;
    if (type != 0x5 && type != 0x6) return "";

    auto [count, extra] = decode_count(off);
    size_t data_start = off + 1 + extra;

    if (type == 0x5) {
        // ASCII string
        if (count > 65536 || data_start + count > len_) return "";
        return std::string(reinterpret_cast<const char*>(data_ + data_start),
                           static_cast<size_t>(count));
    }
    // Unicode \u2014 extract ASCII-compatible chars
    if (count > 65536 || data_start + count * 2 > len_) return "";
    std::string result;
    for (uint64_t i = 0; i < count; i++) {
        uint16_t ch = static_cast<uint16_t>(read_be(data_start + i * 2, 2));
        if (ch < 128) result += static_cast<char>(ch);
    }
    return result;
}

std::vector<uint8_t> BPlistReader::read_data_object(uint64_t obj_idx) const {
    size_t off = static_cast<size_t>(get_offset(obj_idx));
    if (off >= len_) return {};

    uint8_t marker = data_[off];
    if ((marker >> 4) != 0x4) return {};

    auto [count, extra] = decode_count(off);
    size_t data_start = off + 1 + extra;
    if (count > (1u << 24) || data_start + count > len_) return {};

    return std::vector<uint8_t>(data_ + data_start,
                                data_ + data_start + count);
}

BPlistReader::DictInfo BPlistReader::read_dict(uint64_t obj_idx) const {
    DictInfo info;
    size_t off = static_cast<size_t>(get_offset(obj_idx));
    if (off >= len_) return info;

    uint8_t marker = data_[off];
    if ((marker >> 4) != 0xD) return info;

    auto [count, extra] = decode_count(off);
    size_t refs_start = off + 1 + extra;
    if (count > 4096 || refs_start + count * 2 * ref_size_ > len_) return info;

    info.key_refs.resize(count);
    info.val_refs.resize(count);

    for (uint64_t i = 0; i < count; i++) {
        info.key_refs[i] = read_be(refs_start + i * ref_size_, ref_size_);
    }
    for (uint64_t i = 0; i < count; i++) {
        info.val_refs[i] = read_be(refs_start + (count + i) * ref_size_,
                                   ref_size_);
    }
    return info;
}

std::vector<uint64_t> BPlistReader::read_array(uint64_t obj_idx) const {
    size_t off = static_cast<size_t>(get_offset(obj_idx));
    if (off >= len_) return {};

    uint8_t marker = data_[off];
    if ((marker >> 4) != 0xA) return {};

    auto [count, extra] = decode_count(off);
    size_t refs_start = off + 1 + extra;
    if (count > 4096 || refs_start + count * ref_size_ > len_) return {};

    std::vector<uint64_t> refs(count);
    for (uint64_t i = 0; i < count; i++) {
        refs[i] = read_be(refs_start + i * ref_size_, ref_size_);
    }
    return refs;
}

bool BPlistReader::get_uint(const std::string& key, uint64_t& out) const {
    auto dict = read_dict(root_object_);
    for (size_t i = 0; i < dict.key_refs.size(); i++) {
        if (read_string_object(dict.key_refs[i]) == key) {
            out = read_uint_object(dict.val_refs[i]);
            return true;
        }
    }
    return false;
}

bool BPlistReader::get_data(const std::string& key, std::vector<uint8_t>& out) const {
    auto dict = read_dict(root_object_);
    for (size_t i = 0; i < dict.key_refs.size(); i++) {
        if (read_string_object(dict.key_refs[i]) == key) {
            out = read_data_object(dict.val_refs[i]);
            return !out.empty();
        }
    }
    return false;
}

bool BPlistReader::has_key(const std::string& key) const {
    auto dict = read_dict(root_object_);
    for (size_t i = 0; i < dict.key_refs.size(); i++) {
        if (read_string_object(dict.key_refs[i]) == key) return true;
    }
    return false;
}

bool BPlistReader::get_string(const std::string& key, std::string& out) const {
    auto dict = read_dict(root_object_);
    for (size_t i = 0; i < dict.key_refs.size(); i++) {
        if (read_string_object(dict.key_refs[i]) == key) {
            out = read_string_object(dict.val_refs[i]);
            return !out.empty();
        }
    }
    return false;
}

bool BPlistReader::get_streams(std::vector<StreamInfo>& out) const {
    auto root = read_dict(root_object_);
    for (size_t i = 0; i < root.key_refs.size(); i++) {
        if (read_string_object(root.key_refs[i]) != "streams") continue;

        auto arr = read_array(root.val_refs[i]);
        for (auto dict_idx : arr) {
            StreamInfo info;
            auto d = read_dict(dict_idx);
            for (size_t j = 0; j < d.key_refs.size(); j++) {
                std::string k = read_string_object(d.key_refs[j]);
                if (k == "type")
                    info.type = read_uint_object(d.val_refs[j]);
                else if (k == "streamConnectionID")
                    info.stream_connection_id = read_uint_object(d.val_refs[j]);
            }
            out.push_back(info);
        }
        return !out.empty();
    }
    return false;
}

// ===== BPlistWriter =====

int BPlistWriter::add_uint(uint64_t val) {
    std::vector<uint8_t> obj;
    if (val <= 0xFF) {
        obj = {0x10, static_cast<uint8_t>(val)};
    } else if (val <= 0xFFFF) {
        obj = {0x11,
               static_cast<uint8_t>((val >> 8) & 0xFF),
               static_cast<uint8_t>(val & 0xFF)};
    } else if (val <= 0xFFFFFFFF) {
        obj = {0x12,
               static_cast<uint8_t>((val >> 24) & 0xFF),
               static_cast<uint8_t>((val >> 16) & 0xFF),
               static_cast<uint8_t>((val >> 8) & 0xFF),
               static_cast<uint8_t>(val & 0xFF)};
    } else {
        obj.push_back(0x13);
        for (int i = 7; i >= 0; i--)
            obj.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
    int idx = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    return idx;
}

int BPlistWriter::add_real(double val) {
    std::vector<uint8_t> obj;
    obj.push_back(0x23); // type 2 (real), size 3 (8 bytes = double)
    uint64_t bits;
    std::memcpy(&bits, &val, 8);
    for (int i = 7; i >= 0; i--)
        obj.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
    int idx = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    return idx;
}

int BPlistWriter::add_bool(bool val) {
    std::vector<uint8_t> obj;
    obj.push_back(val ? 0x09 : 0x08);
    int idx = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    return idx;
}

int BPlistWriter::add_string(const std::string& str) {
    std::vector<uint8_t> obj;
    size_t slen = str.size();
    if (slen < 15) {
        obj.push_back(static_cast<uint8_t>(0x50 | slen));
    } else {
        obj.push_back(0x5F);
        if (slen <= 0xFF) {
            obj.push_back(0x10);
            obj.push_back(static_cast<uint8_t>(slen));
        } else {
            obj.push_back(0x11);
            obj.push_back(static_cast<uint8_t>((slen >> 8) & 0xFF));
            obj.push_back(static_cast<uint8_t>(slen & 0xFF));
        }
    }
    obj.insert(obj.end(), str.begin(), str.end());
    int idx = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    return idx;
}

int BPlistWriter::add_data(const uint8_t* d, size_t dlen) {
    std::vector<uint8_t> obj;
    if (dlen < 15) {
        obj.push_back(static_cast<uint8_t>(0x40 | dlen));
    } else {
        obj.push_back(0x4F);
        if (dlen <= 0xFF) {
            obj.push_back(0x10);
            obj.push_back(static_cast<uint8_t>(dlen));
        } else {
            obj.push_back(0x11);
            obj.push_back(static_cast<uint8_t>((dlen >> 8) & 0xFF));
            obj.push_back(static_cast<uint8_t>(dlen & 0xFF));
        }
    }
    obj.insert(obj.end(), d, d + dlen);
    int idx = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    return idx;
}

int BPlistWriter::add_array(const std::vector<int>& items) {
    std::vector<uint8_t> obj;
    size_t count = items.size();
    if (count < 15) {
        obj.push_back(static_cast<uint8_t>(0xA0 | count));
    } else {
        obj.push_back(0xAF);
        obj.push_back(0x10);
        obj.push_back(static_cast<uint8_t>(count));
    }
    for (int ref : items) {
        obj.push_back(static_cast<uint8_t>(ref));
    }
    int idx = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    return idx;
}

int BPlistWriter::add_dict(const std::vector<std::pair<int, int>>& entries) {
    std::vector<uint8_t> obj;
    size_t count = entries.size();
    if (count < 15) {
        obj.push_back(static_cast<uint8_t>(0xD0 | count));
    } else {
        obj.push_back(0xDF);
        obj.push_back(0x10);
        obj.push_back(static_cast<uint8_t>(count));
    }
    // Keys first, then values
    for (const auto& [k, v] : entries) {
        obj.push_back(static_cast<uint8_t>(k));
    }
    for (const auto& [k, v] : entries) {
        obj.push_back(static_cast<uint8_t>(v));
    }
    int idx = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    return idx;
}

std::vector<uint8_t> BPlistWriter::build(int root_idx) const {
    std::vector<uint8_t> result;

    // Header: "bplist00"
    result.insert(result.end(), {'b','p','l','i','s','t','0','0'});

    // Write objects and collect offsets
    std::vector<uint64_t> offsets;
    for (const auto& obj : objects_) {
        offsets.push_back(result.size());
        result.insert(result.end(), obj.begin(), obj.end());
    }

    // Offset table
    uint64_t max_offset = result.size();
    uint8_t offset_size = (max_offset <= 0xFF) ? 1 :
                          (max_offset <= 0xFFFF) ? 2 : 4;

    uint64_t offset_table_off = result.size();
    for (uint64_t off : offsets) {
        for (int i = offset_size - 1; i >= 0; i--) {
            result.push_back(static_cast<uint8_t>((off >> (i * 8)) & 0xFF));
        }
    }

    // Trailer: 32 bytes
    uint64_t num_objects = objects_.size();
    uint64_t root = static_cast<uint64_t>(root_idx);

    for (int i = 0; i < 6; i++) result.push_back(0); // unused
    result.push_back(offset_size);
    result.push_back(1); // ref_size = 1 byte (sufficient for < 256 objects)

    // num_objects (8 bytes BE)
    for (int i = 7; i >= 0; i--)
        result.push_back(static_cast<uint8_t>((num_objects >> (i * 8)) & 0xFF));
    // root object (8 bytes BE)
    for (int i = 7; i >= 0; i--)
        result.push_back(static_cast<uint8_t>((root >> (i * 8)) & 0xFF));
    // offset table offset (8 bytes BE)
    for (int i = 7; i >= 0; i--)
        result.push_back(static_cast<uint8_t>((offset_table_off >> (i * 8)) & 0xFF));

    return result;
}

} // namespace openmirror::airplay
