#include "lsmkv/db.h"
#include <cassert>
#include <cstdlib>
#include <functional>

void RegisterTest(const std::string& name, std::function<void()> fn);

namespace {

void TestCompactionAndRangeScan() {
    std::string dbpath = "/tmp/test_lsmkv_compaction";
    std::string rm_cmd = "rm -rf " + dbpath;
    ::system(rm_cmd.c_str());

    lsmkv::Options options;
    options.memtable_size = 16 * 1024; // Small 16KB memtable to trigger multiple flushes
    options.l0_compaction_trigger = 3;

    {
        std::unique_ptr<lsmkv::DB> db;
        assert(lsmkv::DB::Open(options, dbpath, &db).ok());

        // Insert 5000 keys to create multiple SSTables and trigger compaction
        for (int i = 0; i < 5000; ++i) {
            char k[32], v[64];
            snprintf(k, sizeof(k), "key%05d", i);
            snprintf(v, sizeof(v), "data%05d", i);
            assert(db->Put(k, v).ok());
        }

        // Overwrite first 1000 keys
        for (int i = 0; i < 1000; ++i) {
            char k[32], v[64];
            snprintf(k, sizeof(k), "key%05d", i);
            snprintf(v, sizeof(v), "new_data%05d", i);
            assert(db->Put(k, v).ok());
        }

        // Delete keys from 1000 to 1999
        for (int i = 1000; i < 2000; ++i) {
            char k[32];
            snprintf(k, sizeof(k), "key%05d", i);
            assert(db->Delete(k).ok());
        }

        // Verify with Range Scan
        auto iter = db->NewIterator();
        iter->seek_to_first();
        int total_keys = 0;
        while (iter->valid()) {
            total_keys++;
            iter->next();
        }
        // 5000 total - 1000 deleted = 4000 live keys
        assert(total_keys == 4000);

        // Verify seeking
        iter->seek("key00500");
        assert(iter->valid());
        assert(iter->key() == "key00500");
        assert(iter->value() == "new_data00500");
    }

    ::system(rm_cmd.c_str());
}

struct Register {
    Register() {
        RegisterTest("CompactionAndRangeScan", &TestCompactionAndRangeScan);
    }
} reg;

} // namespace
