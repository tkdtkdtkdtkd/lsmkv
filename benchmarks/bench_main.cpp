#include "lsmkv/db.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <algorithm>
#include <cstdlib>

struct BenchmarkResult {
    std::string name;
    double ops_per_sec;
    double p50_us;
    double p95_us;
    double p99_us;
};

void PrintResult(const BenchmarkResult& res) {
    std::cout << std::left << std::setw(24) << res.name
              << std::right << std::setw(14) << static_cast<long>(res.ops_per_sec) << " ops/s"
              << std::setw(12) << std::fixed << std::setprecision(2) << res.p50_us << " us (p50)"
              << std::setw(12) << res.p95_us << " us (p95)"
              << std::setw(12) << res.p99_us << " us (p99)\n";
}

int main(int argc, char** argv) {
    int total_keys = 100000;
    if (argc > 1) {
        total_keys = std::atoi(argv[1]);
    }

    std::string dbpath = "/tmp/lsmkv_bench_data";
    std::string rm_cmd = "rm -rf " + dbpath;
    ::system(rm_cmd.c_str());

    lsmkv::Options options;
    options.memtable_size = 8 * 1024 * 1024; // 8MB
    options.block_size = 4 * 1024;           // 4KB
    options.bloom_bits_per_key = 10;
    options.sync_wal = false;

    std::unique_ptr<lsmkv::DB> db;
    lsmkv::Status s = lsmkv::DB::Open(options, dbpath, &db);
    if (!s.ok()) {
        std::cerr << "Open failed: " << s.message() << "\n";
        return 1;
    }

    std::cout << "=========================================================================\n";
    std::cout << "             LSMKV STORAGE ENGINE BENCHMARK REPORT                      \n";
    std::cout << "=========================================================================\n";
    std::cout << "Keys: " << total_keys << " | Value size: 100 bytes | WAL sync: async\n\n";

    std::vector<std::string> keys;
    keys.reserve(total_keys);
    for (int i = 0; i < total_keys; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "bench_key_%08d", i);
        keys.push_back(buf);
    }

    std::string value_payload(100, 'x');

    // 1. Sequential PUT
    {
        std::vector<double> latencies;
        latencies.reserve(total_keys);
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < total_keys; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            db->Put(keys[i], value_payload);
            auto t1 = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end - start).count();

        std::sort(latencies.begin(), latencies.end());
        BenchmarkResult res{
            "Sequential PUT",
            total_keys / elapsed_sec,
            latencies[total_keys * 0.50],
            latencies[total_keys * 0.95],
            latencies[total_keys * 0.99]
        };
        PrintResult(res);
    }

    // 2. Random PUT (Overwrites)
    {
        std::vector<std::string> rand_keys = keys;
        std::mt19937 g(1337);
        std::shuffle(rand_keys.begin(), rand_keys.end(), g);

        std::vector<double> latencies;
        latencies.reserve(total_keys);
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < total_keys; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            db->Put(rand_keys[i], value_payload);
            auto t1 = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end - start).count();

        std::sort(latencies.begin(), latencies.end());
        BenchmarkResult res{
            "Random PUT",
            total_keys / elapsed_sec,
            latencies[total_keys * 0.50],
            latencies[total_keys * 0.95],
            latencies[total_keys * 0.99]
        };
        PrintResult(res);
    }

    // 3. Random GET (Hits)
    {
        std::vector<std::string> rand_keys = keys;
        std::mt19937 g(42);
        std::shuffle(rand_keys.begin(), rand_keys.end(), g);

        std::vector<double> latencies;
        latencies.reserve(total_keys);
        std::string val;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < total_keys; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            db->Get(rand_keys[i], &val);
            auto t1 = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end - start).count();

        std::sort(latencies.begin(), latencies.end());
        BenchmarkResult res{
            "Random GET (Hits)",
            total_keys / elapsed_sec,
            latencies[total_keys * 0.50],
            latencies[total_keys * 0.95],
            latencies[total_keys * 0.99]
        };
        PrintResult(res);
    }

    // 4. Random GET (Misses - Bloom Filter effectiveness)
    {
        std::vector<double> latencies;
        latencies.reserve(total_keys);
        std::string val;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < total_keys; ++i) {
            char miss_key[32];
            snprintf(miss_key, sizeof(miss_key), "nonexistent_key_%08d", i);
            auto t0 = std::chrono::high_resolution_clock::now();
            db->Get(miss_key, &val);
            auto t1 = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end - start).count();

        std::sort(latencies.begin(), latencies.end());
        BenchmarkResult res{
            "Random GET (Misses)",
            total_keys / elapsed_sec,
            latencies[total_keys * 0.50],
            latencies[total_keys * 0.95],
            latencies[total_keys * 0.99]
        };
        PrintResult(res);
    }

    // 5. Sequential Range Scan
    {
        auto start = std::chrono::high_resolution_clock::now();
        auto iter = db->NewIterator();
        iter->seek_to_first();
        long scan_count = 0;
        while (iter->valid()) {
            scan_count++;
            iter->next();
        }
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end - start).count();

        std::cout << std::left << std::setw(24) << "Range SCAN (All Keys)"
                  << std::right << std::setw(14) << static_cast<long>(scan_count / elapsed_sec) << " ops/s"
                  << "  (" << scan_count << " keys scanned in " << elapsed_sec << " s)\n";
    }

    std::cout << "=========================================================================\n";

    db.reset();
    ::system(rm_cmd.c_str());
    return 0;
}
