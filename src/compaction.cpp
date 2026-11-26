#include "compaction.h"
#include "sstable_builder.h"
#include "sstable_reader.h"
#include <queue>
#include <algorithm>
#include <unistd.h>

namespace lsmkv {

CompactionController::CompactionController(const Options& options) : options_(options) {}

bool CompactionController::NeedsCompaction(const VersionSet& vset, size_t* out_level) const {
    auto current = vset.current();
    if (!current) return false;

    // Trigger L0 compaction if number of files exceeds threshold
    if (current->files(0).size() >= options_.l0_compaction_trigger) {
        if (out_level) *out_level = 0;
        return true;
    }

    // Check level bytes for level >= 1
    double max_score = 0.0;
    size_t best_level = 0;
    uint64_t max_bytes = options_.max_bytes_for_level_base;

    for (size_t lvl = 1; lvl < options_.max_level; ++lvl) {
        uint64_t bytes = current->TotalLevelBytes(lvl);
        double score = static_cast<double>(bytes) / max_bytes;
        if (score > max_score) {
            max_score = score;
            best_level = lvl;
        }
        max_bytes = static_cast<uint64_t>(max_bytes * options_.max_bytes_for_level_multiplier);
    }

    if (max_score >= 1.0) {
        if (out_level) *out_level = best_level;
        return true;
    }

    return false;
}

std::unique_ptr<Compaction> CompactionController::PickCompaction(VersionSet& vset, size_t level) {
    auto current = vset.current();
    if (level >= options_.max_level || current->files(level).empty()) {
        return nullptr;
    }

    auto c = std::make_unique<Compaction>();
    c->level = level;

    if (level == 0) {
        // Compact all L0 files together
        c->inputs[0] = current->files(0);
    } else {
        // Pick first file from level
        c->inputs[0].push_back(current->files(level)[0]);
    }

    // Find key range of inputs[0]
    std::string smallest = c->inputs[0][0]->smallest_key;
    std::string largest = c->inputs[0][0]->largest_key;
    for (const auto& f : c->inputs[0]) {
        if (InternalKey::Compare(f->smallest_key, smallest) < 0) smallest = f->smallest_key;
        if (InternalKey::Compare(f->largest_key, largest) > 0) largest = f->largest_key;
    }

    // Find overlapping files in level + 1
    size_t next_level = level + 1;
    for (const auto& f : current->files(next_level)) {
        if (InternalKey::Compare(f->largest_key, smallest) >= 0 &&
            InternalKey::Compare(f->smallest_key, largest) <= 0) {
            c->inputs[1].push_back(f);
        }
    }

    return c;
}

struct HeapItem {
    std::string key;
    std::string value;
    size_t iterator_index;

    bool operator>(const HeapItem& other) const {
        return InternalKey::Compare(key, other.key) > 0;
    }
};

Status CompactionController::RunCompaction(const Options& options, VersionSet& vset, Compaction* c, VersionEdit* edit) {
    std::vector<std::unique_ptr<Iterator>> iters;
    for (const auto& f : c->inputs[0]) {
        if (f->reader) iters.push_back(f->reader->NewIterator());
    }
    for (const auto& f : c->inputs[1]) {
        if (f->reader) iters.push_back(f->reader->NewIterator());
    }

    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> min_heap;

    for (size_t i = 0; i < iters.size(); ++i) {
        iters[i]->seek_to_first();
        if (iters[i]->valid()) {
            min_heap.push({std::string(iters[i]->key()), std::string(iters[i]->value()), i});
        }
    }

    std::unique_ptr<SSTableBuilder> builder;
    std::string current_user_key;
    bool has_current_user_key = false;
    uint64_t current_file_number = 0;

    auto finish_current_sstable = [&]() -> Status {
        if (builder) {
            Status s = builder->Finish();
            if (!s.ok()) return s;

            auto meta = std::make_shared<FileMetaData>();
            meta->number = current_file_number;
            meta->file_size = builder->FileSize();
            meta->smallest_key = builder->SmallestKey();
            meta->largest_key = builder->LargestKey();

            std::shared_ptr<SSTableReader> reader;
            s = SSTableReader::Open(options, vset.TableFileName(current_file_number), meta->file_size, &reader);
            if (!s.ok()) return s;
            meta->reader = reader;
            reader->SetFileNumber(current_file_number);

            edit->new_files.emplace_back(c->level + 1, meta);
            builder.reset();
        }
        return Status::OK();
    };

    while (!min_heap.empty()) {
        HeapItem top = min_heap.top();
        min_heap.pop();

        std::string_view user_key = ExtractUserKey(top.key);
        ValueType type = ExtractValueType(top.key);

        bool drop = false;
        if (has_current_user_key && user_key == current_user_key) {
            // Drop older versions of the same user key
            drop = true;
        } else {
            has_current_user_key = true;
            current_user_key = std::string(user_key);

            // If it's a deletion and no higher levels contain this user key, we can drop tombstone at bottom
            if (type == ValueType::kTypeDeletion && c->level + 1 >= options.max_level) {
                drop = true;
            }
        }

        if (!drop) {
            if (!builder) {
                current_file_number = vset.NewFileNumber();
                builder = std::make_unique<SSTableBuilder>(options, vset.TableFileName(current_file_number));
            }

            builder->Add(top.key, top.value);

            if (builder->FileSize() >= options.max_bytes_for_level_base) {
                Status s = finish_current_sstable();
                if (!s.ok()) return s;
            }
        }

        // Advance the iterator that provided top
        size_t idx = top.iterator_index;
        iters[idx]->next();
        if (iters[idx]->valid()) {
            min_heap.push({std::string(iters[idx]->key()), std::string(iters[idx]->value()), idx});
        }
    }

    Status s = finish_current_sstable();
    if (!s.ok()) return s;

    // Record deleted files
    for (const auto& f : c->inputs[0]) {
        edit->deleted_files.emplace_back(c->level, f->number);
    }
    for (const auto& f : c->inputs[1]) {
        edit->deleted_files.emplace_back(c->level + 1, f->number);
    }

    return Status::OK();
}

} // namespace lsmkv
