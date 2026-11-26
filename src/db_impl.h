#pragma once

#include "lsmkv/db.h"
#include "memtable.h"
#include "wal.h"
#include "manifest.h"
#include "compaction.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <deque>

namespace lsmkv {

class DBImpl : public DB {
public:
    DBImpl(const Options& options, const std::string& dbname);
    ~DBImpl() override;

    Status Open();

    Status Put(std::string_view key, std::string_view value) override;
    Status Get(std::string_view key, std::string* value) override;
    Status Delete(std::string_view key) override;
    std::unique_ptr<Iterator> NewIterator() override;
    Status Sync() override;

    // Direct flush for testing or memory pressure
    Status FlushMemTable();
    Status CompactRange();

private:
    Status WriteRecord(ValueType type, std::string_view key, std::string_view value);
    Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);
    Status Recover();

    void BackgroundThreadMain();
    void MaybeScheduleCompaction();
    Status BackgroundCompaction();
    Status WriteLevel0Table(MemTable* mem, VersionEdit* edit);

    const Options options_;
    const std::string dbname_;

    std::mutex mutex_;
    std::condition_variable cv_;

    std::shared_ptr<MemTable> mem_;
    std::shared_ptr<MemTable> imm_;
    std::unique_ptr<WalWriter> wal_;
    uint64_t wal_file_number_ = 0;

    VersionSet versions_;
    CompactionController compaction_controller_;

    std::atomic<bool> shutting_down_{false};
    std::thread background_thread_;
    bool background_compaction_scheduled_ = false;

    SequenceNumber last_sequence_{0};
};

} // namespace lsmkv
