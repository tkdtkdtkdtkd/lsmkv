#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

namespace lsmkv {

enum class StatusType {
    kOk,
    kNotFound,
    kCorruption,
    kIOError,
    kInvalidArgument
};

class Status {
public:
    Status() : code_(StatusType::kOk) {}
    Status(StatusType code, std::string msg) : code_(code), msg_(std::move(msg)) {}

    static Status OK() { return Status(); }
    static Status NotFound(std::string msg = "") { return Status(StatusType::kNotFound, std::move(msg)); }
    static Status Corruption(std::string msg = "") { return Status(StatusType::kCorruption, std::move(msg)); }
    static Status IOError(std::string msg = "") { return Status(StatusType::kIOError, std::move(msg)); }
    static Status InvalidArgument(std::string msg = "") { return Status(StatusType::kInvalidArgument, std::move(msg)); }

    bool ok() const { return code_ == StatusType::kOk; }
    bool is_not_found() const { return code_ == StatusType::kNotFound; }
    bool is_corruption() const { return code_ == StatusType::kCorruption; }
    bool is_io_error() const { return code_ == StatusType::kIOError; }

    const std::string& message() const { return msg_; }

private:
    StatusType code_;
    std::string msg_;
};

enum class ValueType : uint8_t {
    kTypeValue = 1,
    kTypeDeletion = 2
};

struct Options {
    size_t memtable_size = 4 * 1024 * 1024; // 4MB
    size_t block_size = 4 * 1024;           // 4KB
    int bloom_bits_per_key = 10;
    bool sync_wal = false;
    size_t l0_compaction_trigger = 4;
    size_t max_level = 4;
    size_t max_bytes_for_level_base = 10 * 1024 * 1024; // 10MB
    double max_bytes_for_level_multiplier = 10.0;
};

struct KeyValue {
    std::string key;
    std::string value;
};

class Iterator {
public:
    virtual ~Iterator() = default;
    virtual bool valid() const = 0;
    virtual void seek_to_first() = 0;
    virtual void seek(std::string_view target) = 0;
    virtual void next() = 0;
    virtual std::string_view key() const = 0;
    virtual std::string_view value() const = 0;
    virtual Status status() const { return Status::OK(); }
};

class DB {
public:
    virtual ~DB() = default;

    static Status Open(const Options& options, const std::string& dbname, std::unique_ptr<DB>* dbptr);

    virtual Status Put(std::string_view key, std::string_view value) = 0;
    virtual Status Get(std::string_view key, std::string* value) = 0;
    virtual Status Delete(std::string_view key) = 0;
    virtual std::unique_ptr<Iterator> NewIterator() = 0;
    virtual Status Sync() = 0;
};

} // namespace lsmkv
