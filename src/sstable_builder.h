#pragma once

#include "lsmkv/db.h"
#include "format.h"
#include <string>
#include <vector>

namespace lsmkv {

struct BlockHandle {
    uint64_t offset = 0;
    uint64_t size = 0;

    void EncodeTo(std::string* dst) const;
    bool DecodeFrom(std::string_view* input);
};

struct Footer {
    BlockHandle index_handle;
    BlockHandle filter_handle;

    static constexpr uint64_t kMagic = 0xdb4775248b80fb57ULL;
    static constexpr size_t kEncodedLength = 2 * (10 + 10) + 8; // Max 2 handles + 8-byte magic

    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(std::string_view input);
};

class BlockBuilder {
public:
    explicit BlockBuilder(size_t block_size);

    void Add(std::string_view key, std::string_view value);
    void Reset();
    std::string Finish();
    size_t CurrentSizeEstimate() const;
    bool Empty() const { return buffer_.empty(); }

private:
    std::string buffer_;
    std::vector<uint32_t> restarts_;
    std::string last_key_;
    int counter_ = 0;
};

class SSTableBuilder {
public:
    SSTableBuilder(const Options& options, const std::string& filename);
    ~SSTableBuilder();

    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;

    void Add(std::string_view key, std::string_view value);
    Status Finish();
    void Abandon();

    uint64_t FileSize() const { return offset_; }
    uint64_t NumEntries() const { return num_entries_; }

    std::string SmallestKey() const { return smallest_key_; }
    std::string LargestKey() const { return largest_key_; }

private:
    void FlushBlock();

    Options options_;
    std::string filename_;
    int fd_ = -1;
    uint64_t offset_ = 0;
    uint64_t num_entries_ = 0;

    BlockBuilder data_block_;
    BlockBuilder index_block_;

    std::string last_key_;
    std::string smallest_key_;
    std::string largest_key_;

    std::vector<std::string> filter_keys_;
};

} // namespace lsmkv
