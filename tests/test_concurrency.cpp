#include "lsmkv/db.h"
#include <cassert>
#include <cstdlib>
#include <thread>
#include <vector>
#include <functional>

void RegisterTest(const std::string& name, std::function<void()> fn);

namespace {

void TestConcurrentReadersAndWriters() {
    std::string dbpath = "/tmp/test_lsmkv_concurrency";
    std::string rm_cmd = "rm -rf " + dbpath;
    ::system(rm_cmd.c_str());

    lsmkv::Options options;
    options.memtable_size = 64 * 1024;

    std::unique_ptr<lsmkv::DB> db;
    assert(lsmkv::DB::Open(options, dbpath, &db).ok());

    const int kNumWriters = 4;
    const int kWritesPerThread = 1000;

    std::vector<std::thread> writers;
    for (int t = 0; t < kNumWriters; ++t) {
        writers.emplace_back([&, t]() {
            for (int i = 0; i < kWritesPerThread; ++i) {
                int key_id = t * kWritesPerThread + i;
                char k[32], v[64];
                snprintf(k, sizeof(k), "concurrent_k%06d", key_id);
                snprintf(v, sizeof(v), "concurrent_v%06d", key_id);
                assert(db->Put(k, v).ok());
            }
        });
    }

    const int kNumReaders = 4;
    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < kNumReaders; ++t) {
        readers.emplace_back([&]() {
            int key_id = 0;
            while (!stop_readers.load(std::memory_order_relaxed)) {
                char k[32];
                snprintf(k, sizeof(k), "concurrent_k%06d", key_id % (kNumWriters * kWritesPerThread));
                std::string val;
                db->Get(k, &val); // May or may not be found yet, must not crash or corrupt
                key_id++;
            }
        });
    }

    for (auto& w : writers) {
        w.join();
    }
    stop_readers.store(true, std::memory_order_relaxed);
    for (auto& r : readers) {
        r.join();
    }

    // Verify all written keys exist
    for (int i = 0; i < kNumWriters * kWritesPerThread; ++i) {
        char k[32], expected_v[64];
        snprintf(k, sizeof(k), "concurrent_k%06d", i);
        snprintf(expected_v, sizeof(expected_v), "concurrent_v%06d", i);
        std::string val;
        lsmkv::Status s = db->Get(k, &val);
        assert(s.ok());
        assert(val == expected_v);
    }

    db.reset();
    ::system(rm_cmd.c_str());
}

struct Register {
    Register() {
        RegisterTest("ConcurrentReadersAndWriters", &TestConcurrentReadersAndWriters);
    }
} reg;

} // namespace
