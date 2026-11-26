#pragma once

#include "lsmkv/db.h"
#include "manifest.h"
#include <memory>
#include <vector>

namespace lsmkv {

struct Compaction {
    size_t level = 0;
    std::vector<std::shared_ptr<FileMetaData>> inputs[2]; // inputs[0] from level, inputs[1] from level+1
};

class CompactionController {
public:
    explicit CompactionController(const Options& options);

    bool NeedsCompaction(const VersionSet& vset, size_t* out_level) const;
    std::unique_ptr<Compaction> PickCompaction(VersionSet& vset, size_t level);

    Status RunCompaction(const Options& options, VersionSet& vset, Compaction* c, VersionEdit* edit);

private:
    Options options_;
};

} // namespace lsmkv
