#pragma once

#include "lsmkv/db.h"
#include "sstable_builder.h"
#include "bloom.h"
#include <memory>
#include <string>

namespace lsmkv {

class Block {
public:
    explicit Block(std::string data);

    class Iterator : public lsmkv::Iterator {
    public:
        explicit Iterator(const Block* block);

        bool valid() const override { return valid_; }
        void seek_to_first() override;
        void seek(std::string_view target) override;
        void next() override;
        std::string_view key() const override { return key_; }
        std::string_view value() const override { return value_; }

    private:
        bool ParseNextEntry();

        const Block* block_;
        uint32_t restart_offset_;
        uint32_t num_restarts_;
        uint32_t current_offset_ = 0;
        uint32_t restart_index_ = 0;
        std::string key_;
        std::string_view value_;
        bool valid_ = false;
    };

    std::unique_ptr<Iterator> NewIterator() const {
        return std::make_unique<Iterator>(this);
    }

private:
    friend class Iterator;
    std::string data_;
    uint32_t restart_offset_;
    uint32_t num_restarts_;
};

class SSTableReader {
public:
    static Status Open(const Options& options, const std::string& filename, uint64_t file_size, std::shared_ptr<SSTableReader>* reader);
    ~SSTableReader();

    Status Get(std::string_view internal_key, std::string* value, Status* read_status);
    std::unique_ptr<Iterator> NewIterator();

    Status ReadBlock(const BlockHandle& handle, std::unique_ptr<Block>* result) const;
    bool KeyMayMatch(std::string_view user_key) const;
    uint64_t FileNumber() const { return file_number_; }
    void SetFileNumber(uint64_t num) { file_number_ = num; }

private:
    SSTableReader(const Options& options, int fd);

    Options options_;
    int fd_ = -1;
    uint64_t file_number_ = 0;
    std::unique_ptr<Block> index_block_;
    std::string filter_data_;
    BloomFilterPolicy filter_policy_;
};

} // namespace lsmkv
