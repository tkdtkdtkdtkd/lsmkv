#include "memtable.h"
#include "coding.h"

namespace lsmkv {

MemTable::MemTable() : comparator_(), table_(comparator_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value) {
    std::string internal_key = MakeInternalKey(key, seq, type);
    // Record layout in skip list: length-prefixed internal key + length-prefixed value
    std::string entry;
    PutLengthPrefixedSlice(&entry, internal_key);
    PutLengthPrefixedSlice(&entry, value);

    table_.Insert(entry);
    approximate_memory_usage_.fetch_add(entry.size() + sizeof(void*) * 4, std::memory_order_relaxed);
}

bool MemTable::Get(std::string_view user_key, SequenceNumber seq, std::string* value, Status* s) {
    std::string lookup_key = MakeInternalKey(user_key, seq, ValueType::kTypeValue);
    std::string entry;
    PutLengthPrefixedSlice(&entry, lookup_key);

    Table::Iterator iter(&table_);
    iter.Seek(entry);
    if (iter.Valid()) {
        std::string_view item = iter.key();
        std::string_view key_slice;
        if (GetLengthPrefixedSlice(&item, &key_slice)) {
            if (ExtractUserKey(key_slice) == user_key) {
                ValueType tag = ExtractValueType(key_slice);
                if (tag == ValueType::kTypeValue) {
                    std::string_view val_slice;
                    GetLengthPrefixedSlice(&item, &val_slice);
                    if (value) *value = std::string(val_slice);
                    *s = Status::OK();
                    return true;
                } else if (tag == ValueType::kTypeDeletion) {
                    *s = Status::NotFound();
                    return true;
                }
            }
        }
    }
    return false;
}

std::string_view MemTable::Iterator::key() const {
    assert(valid());
    std::string_view item = iter_.key();
    std::string_view k;
    GetLengthPrefixedSlice(&item, &k);
    return k;
}

std::string_view MemTable::Iterator::value() const {
    assert(valid());
    std::string_view item = iter_.key();
    std::string_view k;
    GetLengthPrefixedSlice(&item, &k);
    std::string_view v;
    GetLengthPrefixedSlice(&item, &v);
    return v;
}

} // namespace lsmkv
