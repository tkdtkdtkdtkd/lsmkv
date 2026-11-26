#pragma once

#include "lsmkv/db.h"
#include "format.h"
#include "sstable_reader.h"
#include <unistd.h>
#include <vector>
#include <memory>
#include <string>

namespace lsmkv {

struct FileMetaData {
    uint64_t number = 0;
    uint64_t file_size = 0;
    std::string smallest_key;
    std::string largest_key;
    std::shared_ptr<SSTableReader> reader;
};

// Represents an immutable snapshot of all levels of SSTables
class Version {
public:
    explicit Version(size_t max_level);

    Status Get(const Options& options, std::string_view user_key, SequenceNumber seq, std::string* value);

    const std::vector<std::shared_ptr<FileMetaData>>& files(size_t level) const {
        return files_[level];
    }
    std::vector<std::shared_ptr<FileMetaData>>& files(size_t level) {
        return files_[level];
    }

    size_t NumLevels() const { return files_.size(); }
    uint64_t TotalLevelBytes(size_t level) const;

private:
    std::vector<std::vector<std::shared_ptr<FileMetaData>>> files_;
};

struct VersionEdit {
    std::vector<std::pair<size_t, std::shared_ptr<FileMetaData>>> new_files;
    std::vector<std::pair<size_t, uint64_t>> deleted_files;
    uint64_t log_number = 0;
    uint64_t next_file_number = 0;
    SequenceNumber last_sequence = 0;

    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(std::string_view src);
};

class VersionSet {
public:
    VersionSet(const std::string& dbname, const Options& options);
    ~VersionSet() {
        if (manifest_fd_ >= 0) {
            ::close(manifest_fd_);
            manifest_fd_ = -1;
        }
    }

    Status Recover();
    Status LogAndApply(VersionEdit* edit);

    std::shared_ptr<Version> current() const { return current_; }

    uint64_t NewFileNumber() { return next_file_number_++; }
    uint64_t NextFileNumber() const { return next_file_number_; }
    void ReuseFileNumber(uint64_t) {}

    SequenceNumber LastSequence() const { return last_sequence_; }
    void SetLastSequence(SequenceNumber s) {
        if (s > last_sequence_) last_sequence_ = s;
    }

    uint64_t LogNumber() const { return log_number_; }
    void SetLogNumber(uint64_t n) { log_number_ = n; }

    std::string TableFileName(uint64_t number) const;
    std::string WalFileName(uint64_t number) const;
    std::string ManifestFileName(uint64_t number) const;
    std::string CurrentFileName() const;

private:
    std::string dbname_;
    Options options_;
    uint64_t next_file_number_ = 1;
    uint64_t manifest_file_number_ = 0;
    uint64_t log_number_ = 0;
    SequenceNumber last_sequence_ = 0;
    int manifest_fd_ = -1;
    std::shared_ptr<Version> current_;
};

} // namespace lsmkv
