#include "db_impl.h"
#include "sstable_builder.h"
#include "sstable_reader.h"
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <queue>
#include <iostream>

namespace lsmkv {

Status DB::Open(const Options& options, const std::string& dbname, std::unique_ptr<DB>* dbptr) {
    ::mkdir(dbname.c_str(), 0755);

    auto impl = std::make_unique<DBImpl>(options, dbname);
    Status s = impl->Open();
    if (!s.ok()) {
        return s;
    }
    *dbptr = std::move(impl);
    return Status::OK();
}

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : options_(options),
      dbname_(dbname),
      versions_(dbname, options),
      compaction_controller_(options) {}

DBImpl::~DBImpl() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        shutting_down_.store(true, std::memory_order_release);
        cv_.notify_all();
    }
    if (background_thread_.joinable()) {
        background_thread_.join();
    }
    // Flush any pending active memtable or sync WAL before shutdown
    if (wal_) {
        wal_->Sync();
        wal_->Close();
    }
}

Status DBImpl::Open() {
    std::unique_lock<std::mutex> lock(mutex_);

    Status s = versions_.Recover();
    if (!s.ok()) return s;

    last_sequence_ = versions_.LastSequence();

    s = Recover();
    if (!s.ok()) return s;

    background_thread_ = std::thread(&DBImpl::BackgroundThreadMain, this);
    return Status::OK();
}

Status DBImpl::Recover() {
    mem_ = std::make_shared<MemTable>();

    // Discover all WAL files present in db directory and replay ones >= versions_.LogNumber()
    std::vector<uint64_t> wal_files;
    DIR* dir = ::opendir(dbname_.c_str());
    if (dir != nullptr) {
        struct dirent* entry;
        while ((entry = ::readdir(dir)) != nullptr) {
            unsigned long long fnum = 0;
            if (sscanf(entry->d_name, "%llu.wal", &fnum) == 1) {
                if (fnum >= versions_.LogNumber()) {
                    wal_files.push_back(fnum);
                }
            }
        }
        ::closedir(dir);
    }
    std::sort(wal_files.begin(), wal_files.end());

    for (uint64_t fnum : wal_files) {
        std::string wal_path = versions_.WalFileName(fnum);
        WalReader reader(wal_path);
        WalReader::Record rec;
        Status s;
        while (reader.ReadRecord(&rec, &s)) {
            mem_->Add(rec.seq, rec.type, rec.key, rec.value);
            if (rec.seq > last_sequence_) {
                last_sequence_ = rec.seq;
            }
        }
    }

    wal_file_number_ = versions_.NewFileNumber();
    wal_ = std::make_unique<WalWriter>(versions_.WalFileName(wal_file_number_), options_.sync_wal);

    VersionEdit edit;
    edit.log_number = wal_file_number_;
    edit.next_file_number = versions_.NextFileNumber();
    edit.last_sequence = last_sequence_;
    return versions_.LogAndApply(&edit);
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
    while (true) {
        if (mem_->ApproximateMemoryUsage() < options_.memtable_size) {
            return Status::OK();
        }
        if (imm_ != nullptr) {
            // Wait for immutable MemTable to be flushed
            cv_.wait(lock);
        } else {
            // Freeze active MemTable and switch to new WAL
            imm_ = mem_;
            mem_ = std::make_shared<MemTable>();

            uint64_t new_wal_num = versions_.NewFileNumber();
            auto new_wal = std::make_unique<WalWriter>(versions_.WalFileName(new_wal_num), options_.sync_wal);
            wal_ = std::move(new_wal);
            wal_file_number_ = new_wal_num;

            MaybeScheduleCompaction();
        }
    }
}

Status DBImpl::WriteRecord(ValueType type, std::string_view key, std::string_view value) {
    std::unique_lock<std::mutex> lock(mutex_);

    Status s = MakeRoomForWrite(lock);
    if (!s.ok()) return s;

    SequenceNumber seq = ++last_sequence_;
    s = wal_->AddRecord(seq, type, key, value);
    if (!s.ok()) return s;

    mem_->Add(seq, type, key, value);
    versions_.SetLastSequence(seq);

    return Status::OK();
}

Status DBImpl::Put(std::string_view key, std::string_view value) {
    return WriteRecord(ValueType::kTypeValue, key, value);
}

Status DBImpl::Delete(std::string_view key) {
    return WriteRecord(ValueType::kTypeDeletion, key, "");
}

Status DBImpl::Sync() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (wal_) {
        return wal_->Sync();
    }
    return Status::OK();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
    std::shared_ptr<MemTable> mem;
    std::shared_ptr<MemTable> imm;
    std::shared_ptr<Version> current;
    SequenceNumber snapshot_seq;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        mem = mem_;
        imm = imm_;
        current = versions_.current();
        snapshot_seq = last_sequence_;
    }

    // 1. Search active MemTable
    Status s;
    if (mem->Get(key, snapshot_seq, value, &s)) {
        return s;
    }

    // 2. Search immutable MemTable
    if (imm != nullptr) {
        if (imm->Get(key, snapshot_seq, value, &s)) {
            return s;
        }
    }

    // 3. Search on-disk SSTables through current Version
    return current->Get(options_, key, snapshot_seq, value);
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit) {
    uint64_t file_num = versions_.NewFileNumber();
    std::string filename = versions_.TableFileName(file_num);

    SSTableBuilder builder(options_, filename);
    auto it = mem->NewIterator();
    for (it->seek_to_first(); it->valid(); it->next()) {
        builder.Add(it->key(), it->value());
    }

    Status s = builder.Finish();
    if (!s.ok()) {
        builder.Abandon();
        return s;
    }

    auto meta = std::make_shared<FileMetaData>();
    meta->number = file_num;
    meta->file_size = builder.FileSize();
    meta->smallest_key = builder.SmallestKey();
    meta->largest_key = builder.LargestKey();

    std::shared_ptr<SSTableReader> reader;
    s = SSTableReader::Open(options_, filename, meta->file_size, &reader);
    if (!s.ok()) return s;
    meta->reader = reader;
    reader->SetFileNumber(file_num);

    edit->new_files.emplace_back(0, meta);
    return Status::OK();
}

void DBImpl::MaybeScheduleCompaction() {
    if (background_compaction_scheduled_ || shutting_down_.load(std::memory_order_relaxed)) {
        return;
    }
    size_t level = 0;
    if (imm_ != nullptr || compaction_controller_.NeedsCompaction(versions_, &level)) {
        background_compaction_scheduled_ = true;
        cv_.notify_all();
    }
}

void DBImpl::BackgroundThreadMain() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutting_down_.load(std::memory_order_acquire)) {
        while (!background_compaction_scheduled_ && !shutting_down_.load(std::memory_order_relaxed)) {
            cv_.wait(lock);
        }
        if (shutting_down_.load(std::memory_order_relaxed)) break;

        BackgroundCompaction();
        background_compaction_scheduled_ = false;
        MaybeScheduleCompaction();
        cv_.notify_all();
    }
}

Status DBImpl::BackgroundCompaction() {
    if (imm_ != nullptr) {
        VersionEdit edit;
        edit.log_number = wal_file_number_;
        Status s = WriteLevel0Table(imm_.get(), &edit);
        if (!s.ok()) return s;

        s = versions_.LogAndApply(&edit);
        if (!s.ok()) return s;

        imm_.reset();
        return Status::OK();
    }

    size_t level = 0;
    if (compaction_controller_.NeedsCompaction(versions_, &level)) {
        auto c = compaction_controller_.PickCompaction(versions_, level);
        if (c) {
            VersionEdit edit;
            edit.log_number = wal_file_number_;
            Status s = compaction_controller_.RunCompaction(options_, versions_, c.get(), &edit);
            if (s.ok()) {
                versions_.LogAndApply(&edit);
                // Unlink old files
                for (const auto& d : edit.deleted_files) {
                    ::unlink(versions_.TableFileName(d.second).c_str());
                }
            }
            return s;
        }
    }

    return Status::OK();
}

Status DBImpl::FlushMemTable() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (mem_->ApproximateMemoryUsage() == 0) return Status::OK();

    while (imm_ != nullptr) {
        cv_.wait(lock);
    }
    imm_ = mem_;
    mem_ = std::make_shared<MemTable>();

    uint64_t new_wal_num = versions_.NewFileNumber();
    wal_ = std::make_unique<WalWriter>(versions_.WalFileName(new_wal_num), options_.sync_wal);
    wal_file_number_ = new_wal_num;

    VersionEdit edit;
    edit.log_number = wal_file_number_;
    Status s = WriteLevel0Table(imm_.get(), &edit);
    if (!s.ok()) return s;

    s = versions_.LogAndApply(&edit);
    imm_.reset();
    cv_.notify_all();
    return s;
}

Status DBImpl::CompactRange() {
    std::unique_lock<std::mutex> lock(mutex_);
    size_t level = 0;
    while (compaction_controller_.NeedsCompaction(versions_, &level)) {
        auto c = compaction_controller_.PickCompaction(versions_, level);
        if (!c) break;
        VersionEdit edit;
        Status s = compaction_controller_.RunCompaction(options_, versions_, c.get(), &edit);
        if (!s.ok()) return s;
        versions_.LogAndApply(&edit);
        for (const auto& d : edit.deleted_files) {
            ::unlink(versions_.TableFileName(d.second).c_str());
        }
    }
    return Status::OK();
}

// Merged iterator combining active MemTable, immutable MemTable, and SSTables
class MergingDBSanIterator : public Iterator {
public:
    MergingDBSanIterator(std::vector<std::unique_ptr<Iterator>> iters)
        : iters_(std::move(iters)) {}

    bool valid() const override { return valid_; }

    void seek_to_first() override {
        for (auto& it : iters_) {
            it->seek_to_first();
        }
        FindSmallest();
        SkipToNextUserKey();
    }

    void seek(std::string_view target) override {
        std::string internal_target = MakeInternalKey(target, kMaxSequenceNumber, ValueType::kTypeValue);
        for (auto& it : iters_) {
            it->seek(internal_target);
        }
        FindSmallest();
        SkipToNextUserKey();
    }

    void next() override {
        assert(valid());
        std::string current_key(current_user_key_);
        // Advance all iterators matching this user key
        while (valid_ && current_user_key_ == current_key) {
            iters_[current_iter_idx_]->next();
            FindSmallest();
        }
        SkipToNextUserKey();
    }

    std::string_view key() const override {
        assert(valid());
        return current_user_key_;
    }

    std::string_view value() const override {
        assert(valid());
        return current_value_;
    }

private:
    void FindSmallest() {
        int best_idx = -1;
        std::string_view best_key;

        for (size_t i = 0; i < iters_.size(); ++i) {
            if (iters_[i]->valid()) {
                if (best_idx == -1 || InternalKey::Compare(iters_[i]->key(), best_key) < 0) {
                    best_idx = static_cast<int>(i);
                    best_key = iters_[i]->key();
                }
            }
        }

        if (best_idx == -1) {
            valid_ = false;
        } else {
            valid_ = true;
            current_iter_idx_ = best_idx;
            current_internal_key_ = std::string(iters_[best_idx]->key());
            current_user_key_ = std::string(ExtractUserKey(current_internal_key_));
            current_value_ = std::string(iters_[best_idx]->value());
        }
    }

    void SkipToNextUserKey() {
        while (valid_) {
            ValueType type = ExtractValueType(current_internal_key_);
            if (type == ValueType::kTypeValue) {
                return; // Valid live key
            }
            // Tombstone: advance past all versions of this user key
            std::string del_key(current_user_key_);
            while (valid_ && current_user_key_ == del_key) {
                iters_[current_iter_idx_]->next();
                FindSmallest();
            }
        }
    }

    std::vector<std::unique_ptr<Iterator>> iters_;
    int current_iter_idx_ = -1;
    std::string current_internal_key_;
    std::string current_user_key_;
    std::string current_value_;
    bool valid_ = false;
};

std::unique_ptr<Iterator> DBImpl::NewIterator() {
    std::unique_lock<std::mutex> lock(mutex_);
    std::vector<std::unique_ptr<Iterator>> iters;

    iters.push_back(mem_->NewIterator());
    if (imm_ != nullptr) {
        iters.push_back(imm_->NewIterator());
    }

    auto current = versions_.current();
    for (size_t lvl = 0; lvl < current->NumLevels(); ++lvl) {
        for (const auto& file : current->files(lvl)) {
            if (file->reader) {
                iters.push_back(file->reader->NewIterator());
            }
        }
    }

    return std::make_unique<MergingDBSanIterator>(std::move(iters));
}

} // namespace lsmkv
