#pragma once

#include "lsmkv/db.h"
#include "format.h"
#include "coding.h"
#include "skiplist.h"
#include <atomic>
#include <memory>
#include <string>

namespace lsmkv {

class MemTable {
public:
    struct KeyComparator {
        int operator()(std::string_view a, std::string_view b) const {
            std::string_view a_key;
            GetLengthPrefixedSlice(&a, &a_key);
            std::string_view b_key;
            GetLengthPrefixedSlice(&b, &b_key);
            return InternalKey::Compare(a_key, b_key);
        }
    };

    using Table = SkipList<std::string, KeyComparator>;

    MemTable();
    ~MemTable() = default;

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);
    bool Get(std::string_view user_key, SequenceNumber seq, std::string* value, Status* s);

    size_t ApproximateMemoryUsage() const {
        return approximate_memory_usage_.load(std::memory_order_relaxed);
    }

    class Iterator : public lsmkv::Iterator {
    public:
        explicit Iterator(const MemTable* mem) : iter_(mem->table_.NewIterator()) {}

        bool valid() const override { return iter_.Valid(); }
        void seek_to_first() override { iter_.SeekToFirst(); }
        void seek(std::string_view target) override {
            std::string entry;
            PutLengthPrefixedSlice(&entry, target);
            iter_.Seek(entry);
        }
        void next() override { iter_.Next(); }
        std::string_view key() const override;
        std::string_view value() const override;

    private:
        Table::Iterator iter_;
    };

    std::unique_ptr<Iterator> NewIterator() const {
        return std::make_unique<Iterator>(this);
    }

private:
    KeyComparator comparator_;
    Table table_;
    std::atomic<size_t> approximate_memory_usage_{0};
};

} // namespace lsmkv
