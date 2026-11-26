#include "lsmkv/db.h"
#include <cassert>
#include <cstdlib>
#include <functional>

void RegisterTest(const std::string& name, std::function<void()> fn);

namespace {

void TestCrashRecovery() {
    std::string dbpath = "/tmp/test_lsmkv_recovery";
    std::string rm_cmd = "rm -rf " + dbpath;
    ::system(rm_cmd.c_str());

    lsmkv::Options options;
    options.memtable_size = 32 * 1024; // 32KB
    options.sync_wal = true;

    {
        std::unique_ptr<lsmkv::DB> db;
        assert(lsmkv::DB::Open(options, dbpath, &db).ok());

        for (int i = 0; i < 2000; ++i) {
            char k[32], v[64];
            snprintf(k, sizeof(k), "k%06d", i);
            snprintf(v, sizeof(v), "val%06d", i);
            assert(db->Put(k, v).ok());
        }

        // Delete odd keys
        for (int i = 1; i < 2000; i += 2) {
            char k[32];
            snprintf(k, sizeof(k), "k%06d", i);
            assert(db->Delete(k).ok());
        }

        // Simulate crash without proper flush by destroying DB
    }

    // Reopen database and verify all data recovered cleanly from WAL and SSTables
    {
        std::unique_ptr<lsmkv::DB> db;
        assert(lsmkv::DB::Open(options, dbpath, &db).ok());

        for (int i = 0; i < 2000; ++i) {
            char k[32];
            snprintf(k, sizeof(k), "k%06d", i);
            std::string val;
            lsmkv::Status s = db->Get(k, &val);
            if (i % 2 == 0) {
                assert(s.ok());
                char expected[64];
                snprintf(expected, sizeof(expected), "val%06d", i);
                assert(val == expected);
            } else {
                assert(s.is_not_found());
            }
        }
    }

    ::system(rm_cmd.c_str());
}

struct Register {
    Register() {
        RegisterTest("CrashRecovery", &TestCrashRecovery);
    }
} reg;

} // namespace
