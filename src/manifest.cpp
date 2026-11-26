#include "manifest.h"
#include "coding.h"
#include "crc32.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>
#include <algorithm>

namespace lsmkv {

Version::Version(size_t max_level) : files_(max_level + 1) {}

uint64_t Version::TotalLevelBytes(size_t level) const {
    if (level >= files_.size()) return 0;
    uint64_t total = 0;
    for (const auto& f : files_[level]) {
        total += f->file_size;
    }
    return total;
}

Status Version::Get(const Options& options, std::string_view user_key, SequenceNumber seq, std::string* value) {
    (void)options;
    std::string lookup_internal_key = MakeInternalKey(user_key, seq, ValueType::kTypeValue);

    // Level 0 files may have overlapping key ranges, searched newest to oldest
    for (auto it = files_[0].rbegin(); it != files_[0].rend(); ++it) {
        auto& meta = *it;
        if (meta->reader && meta->reader->KeyMayMatch(user_key)) {
            Status read_status;
            Status s = meta->reader->Get(lookup_internal_key, value, &read_status);
            if (s.ok()) return Status::OK();
            if (read_status.is_not_found()) return Status::NotFound(); // found explicit tombstone
            if (!read_status.ok()) return read_status;
        }
    }

    // Level >= 1 files are non-overlapping and sorted by user key
    for (size_t level = 1; level < files_.size(); ++level) {
        if (files_[level].empty()) continue;

        for (const auto& meta : files_[level]) {
            if (InternalKey::Compare(meta->largest_key, lookup_internal_key) < 0) {
                continue;
            }
            if (ExtractUserKey(meta->smallest_key) > user_key) {
                break;
            }
            if (meta->reader && meta->reader->KeyMayMatch(user_key)) {
                Status read_status;
                Status s = meta->reader->Get(lookup_internal_key, value, &read_status);
                if (s.ok()) return Status::OK();
                if (read_status.is_not_found()) return Status::NotFound();
                if (!read_status.ok()) return read_status;
            }
        }
    }

    return Status::NotFound();
}

void VersionEdit::EncodeTo(std::string* dst) const {
    PutVarint64(dst, log_number);
    PutVarint64(dst, next_file_number);
    PutVarint64(dst, last_sequence);

    // Deleted files
    PutVarint32(dst, static_cast<uint32_t>(deleted_files.size()));
    for (const auto& d : deleted_files) {
        PutVarint32(dst, static_cast<uint32_t>(d.first));
        PutVarint64(dst, d.second);
    }

    // New files
    PutVarint32(dst, static_cast<uint32_t>(new_files.size()));
    for (const auto& n : new_files) {
        PutVarint32(dst, static_cast<uint32_t>(n.first));
        PutVarint64(dst, n.second->number);
        PutVarint64(dst, n.second->file_size);
        PutLengthPrefixedSlice(dst, n.second->smallest_key);
        PutLengthPrefixedSlice(dst, n.second->largest_key);
    }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
    std::string_view input = src;
    if (!GetVarint64(&input, &log_number) ||
        !GetVarint64(&input, &next_file_number) ||
        !GetVarint64(&input, &last_sequence)) {
        return Status::Corruption("Invalid VersionEdit header");
    }

    uint32_t num_del = 0;
    if (!GetVarint32(&input, &num_del)) return Status::Corruption("Bad deleted files count");
    deleted_files.clear();
    for (uint32_t i = 0; i < num_del; ++i) {
        uint32_t level = 0;
        uint64_t file_num = 0;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &file_num)) {
            return Status::Corruption("Bad deleted file item");
        }
        deleted_files.emplace_back(level, file_num);
    }

    uint32_t num_new = 0;
    if (!GetVarint32(&input, &num_new)) return Status::Corruption("Bad new files count");
    new_files.clear();
    for (uint32_t i = 0; i < num_new; ++i) {
        uint32_t level = 0;
        auto meta = std::make_shared<FileMetaData>();
        std::string_view sm, lg;
        if (!GetVarint32(&input, &level) ||
            !GetVarint64(&input, &meta->number) ||
            !GetVarint64(&input, &meta->file_size) ||
            !GetLengthPrefixedSlice(&input, &sm) ||
            !GetLengthPrefixedSlice(&input, &lg)) {
            return Status::Corruption("Bad new file item");
        }
        meta->smallest_key = std::string(sm);
        meta->largest_key = std::string(lg);
        new_files.emplace_back(level, meta);
    }
    return Status::OK();
}

VersionSet::VersionSet(const std::string& dbname, const Options& options)
    : dbname_(dbname), options_(options), current_(std::make_shared<Version>(options.max_level)) {}

std::string VersionSet::TableFileName(uint64_t number) const {
    char buf[64];
    snprintf(buf, sizeof(buf), "/%06llu.sst", static_cast<unsigned long long>(number));
    return dbname_ + buf;
}

std::string VersionSet::WalFileName(uint64_t number) const {
    char buf[64];
    snprintf(buf, sizeof(buf), "/%06llu.wal", static_cast<unsigned long long>(number));
    return dbname_ + buf;
}

std::string VersionSet::ManifestFileName(uint64_t number) const {
    char buf[64];
    snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
    return dbname_ + buf;
}

std::string VersionSet::CurrentFileName() const {
    return dbname_ + "/CURRENT";
}

Status VersionSet::Recover() {
    std::string current_path = CurrentFileName();
    int cfd = ::open(current_path.c_str(), O_RDONLY);
    if (cfd < 0) {
        // First startup: empty database
        return Status::OK();
    }

    char manifest_name[256];
    ssize_t n = ::read(cfd, manifest_name, sizeof(manifest_name) - 1);
    ::close(cfd);
    if (n <= 0) {
        return Status::Corruption("CURRENT file empty");
    }
    while (n > 0 && (manifest_name[n - 1] == '\n' || manifest_name[n - 1] == '\r')) {
        n--;
    }
    manifest_name[n] = '\0';

    // Parse manifest file number from name: MANIFEST-000012
    unsigned long long m_num = 0;
    if (sscanf(manifest_name, "MANIFEST-%llu", &m_num) == 1) {
        manifest_file_number_ = m_num;
    }

    std::string manifest_path = dbname_ + "/" + manifest_name;
    int mfd = ::open(manifest_path.c_str(), O_RDWR | O_APPEND);
    if (mfd < 0) {
        mfd = ::open(manifest_path.c_str(), O_RDONLY);
    }
    if (mfd < 0) {
        return Status::Corruption("Cannot open MANIFEST: " + manifest_path);
    }
    manifest_fd_ = mfd;

    auto v = std::make_shared<Version>(options_.max_level);

    // Read edits sequentially from manifest
    ::lseek(mfd, 0, SEEK_SET);
    while (true) {
        char header[8];
        ssize_t hr = ::read(mfd, header, 8);
        if (hr == 0) break;
        if (hr < 8) break;

        uint32_t crc = DecodeFixed32(header);
        uint32_t len = DecodeFixed32(header + 4);

        std::string record_buf;
        record_buf.resize(len);
        if (::read(mfd, &record_buf[0], len) != static_cast<ssize_t>(len)) break;

        if (crc32(record_buf.data(), len) != crc) {
            break; // Stop replay at last valid record
        }

        VersionEdit edit;
        if (edit.DecodeFrom(record_buf).ok()) {
            if (edit.log_number > 0) log_number_ = edit.log_number;
            if (edit.next_file_number > 0) next_file_number_ = edit.next_file_number;
            if (edit.last_sequence > 0) last_sequence_ = edit.last_sequence;

            for (const auto& del : edit.deleted_files) {
                size_t lvl = del.first;
                uint64_t fnum = del.second;
                auto& flist = v->files(lvl);
                flist.erase(std::remove_if(flist.begin(), flist.end(),
                    [fnum](const std::shared_ptr<FileMetaData>& f) {
                        return f->number == fnum;
                    }), flist.end());
            }

            for (const auto& nfile : edit.new_files) {
                size_t lvl = nfile.first;
                // Open SSTableReader
                std::shared_ptr<SSTableReader> reader;
                std::string sst_path = TableFileName(nfile.second->number);
                Status s = SSTableReader::Open(options_, sst_path, nfile.second->file_size, &reader);
                if (s.ok()) {
                    nfile.second->reader = reader;
                    reader->SetFileNumber(nfile.second->number);
                    v->files(lvl).push_back(nfile.second);
                }
            }
        }
    }
    // Keep manifest_fd_ open for appending future edits
    // Do NOT close(mfd);

    // Sort files in levels >= 1 by smallest key
    for (size_t lvl = 1; lvl < v->NumLevels(); ++lvl) {
        std::sort(v->files(lvl).begin(), v->files(lvl).end(),
            [](const std::shared_ptr<FileMetaData>& a, const std::shared_ptr<FileMetaData>& b) {
                return InternalKey::Compare(a->smallest_key, b->smallest_key) < 0;
            });
    }

    current_ = std::move(v);
    return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
    if (edit->next_file_number == 0) {
        edit->next_file_number = next_file_number_;
    }
    if (edit->last_sequence == 0) {
        edit->last_sequence = last_sequence_;
    }

    // Apply to new Version
    auto new_v = std::make_shared<Version>(options_.max_level);
    for (size_t lvl = 0; lvl <= options_.max_level; ++lvl) {
        new_v->files(lvl) = current_->files(lvl);
    }

    for (const auto& d : edit->deleted_files) {
        auto& flist = new_v->files(d.first);
        flist.erase(std::remove_if(flist.begin(), flist.end(),
            [&d](const std::shared_ptr<FileMetaData>& f) {
                return f->number == d.second;
            }), flist.end());
    }

    for (const auto& n : edit->new_files) {
        new_v->files(n.first).push_back(n.second);
    }

    for (size_t lvl = 1; lvl <= options_.max_level; ++lvl) {
        std::sort(new_v->files(lvl).begin(), new_v->files(lvl).end(),
            [](const std::shared_ptr<FileMetaData>& a, const std::shared_ptr<FileMetaData>& b) {
                return InternalKey::Compare(a->smallest_key, b->smallest_key) < 0;
            });
    }

    // Serialize edit
    std::string record;
    edit->EncodeTo(&record);

    uint32_t crc = crc32(record.data(), record.size());
    std::string to_write;
    PutFixed32(&to_write, crc);
    PutFixed32(&to_write, static_cast<uint32_t>(record.size()));
    to_write.append(record);

    if (manifest_fd_ < 0) {
        manifest_file_number_ = NewFileNumber();
        std::string mpath = ManifestFileName(manifest_file_number_);
        manifest_fd_ = ::open(mpath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (manifest_fd_ < 0) {
            return Status::IOError("Failed to open MANIFEST for writing");
        }

        // Point CURRENT to new manifest file via atomic temporary rename
        std::string tmp_current = dbname_ + "/CURRENT.tmp";
        char mname[64];
        snprintf(mname, sizeof(mname), "MANIFEST-%06llu\n", static_cast<unsigned long long>(manifest_file_number_));
        int cfd = ::open(tmp_current.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ::write(cfd, mname, strlen(mname));
        ::fsync(cfd);
        ::close(cfd);

        ::rename(tmp_current.c_str(), CurrentFileName().c_str());
    }

    if (::write(manifest_fd_, to_write.data(), to_write.size()) != static_cast<ssize_t>(to_write.size())) {
        return Status::IOError("Failed to write VersionEdit to MANIFEST");
    }
    ::fsync(manifest_fd_);

    current_ = std::move(new_v);
    if (edit->log_number > 0) log_number_ = edit->log_number;
    if (edit->next_file_number > 0) next_file_number_ = edit->next_file_number;
    if (edit->last_sequence > 0) last_sequence_ = edit->last_sequence;

    return Status::OK();
}

} // namespace lsmkv
