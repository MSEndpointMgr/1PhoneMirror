#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace openmirror::airplay {

// Minimal Apple binary plist reader for AirPlay SETUP messages
class BPlistReader {
public:
    bool parse(const uint8_t* data, size_t len);

    // Query the root dict
    bool get_uint(const std::string& key, uint64_t& out) const;
    bool get_data(const std::string& key, std::vector<uint8_t>& out) const;
    bool has_key(const std::string& key) const;

    struct StreamInfo {
        uint64_t type = 0;
        uint64_t stream_connection_id = 0;
    };
    bool get_streams(std::vector<StreamInfo>& out) const;

private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;

    uint8_t offset_size_ = 0;
    uint8_t ref_size_ = 0;
    uint64_t num_objects_ = 0;
    uint64_t root_object_ = 0;
    uint64_t offset_table_off_ = 0;
    std::vector<uint64_t> offsets_;

    uint64_t read_be(size_t offset, int bytes) const;
    uint64_t get_offset(uint64_t obj_idx) const;
    std::pair<uint64_t, size_t> decode_count(size_t offset) const;

    uint64_t read_uint_object(uint64_t obj_idx) const;
    std::string read_string_object(uint64_t obj_idx) const;
    std::vector<uint8_t> read_data_object(uint64_t obj_idx) const;

    struct DictInfo {
        std::vector<uint64_t> key_refs;
        std::vector<uint64_t> val_refs;
    };
    DictInfo read_dict(uint64_t obj_idx) const;
    std::vector<uint64_t> read_array(uint64_t obj_idx) const;
};

// Minimal Apple binary plist writer for AirPlay SETUP responses
class BPlistWriter {
public:
    int add_uint(uint64_t val);
    int add_real(double val);
    int add_bool(bool val);
    int add_string(const std::string& str);
    int add_data(const uint8_t* d, size_t len);
    int add_array(const std::vector<int>& items);
    int add_dict(const std::vector<std::pair<int, int>>& entries);

    std::vector<uint8_t> build(int root_idx) const;

private:
    std::vector<std::vector<uint8_t>> objects_;
};

} // namespace openmirror::airplay
