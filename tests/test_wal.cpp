#include "wal.h"
#include <cassert>
#include <unistd.h>
#include <functional>

void RegisterTest(const std::string& name, std::function<void()> fn);

namespace {

void TestWalAppendAndRead() {
    std::string path = "/tmp/test_wal_append.wal";
    ::unlink(path.c_str());

    {
        lsmkv::WalWriter writer(path, true);
        assert(writer.AddRecord(1, lsmkv::ValueType::kTypeValue, "k1", "v1").ok());
        assert(writer.AddRecord(2, lsmkv::ValueType::kTypeValue, "k2", "v2").ok());
        assert(writer.AddRecord(3, lsmkv::ValueType::kTypeDeletion, "k1", "").ok());
    }

    {
        lsmkv::WalReader reader(path);
        lsmkv::WalReader::Record rec;
        lsmkv::Status s;

        assert(reader.ReadRecord(&rec, &s) && s.ok());
        assert(rec.seq == 1 && rec.type == lsmkv::ValueType::kTypeValue && rec.key == "k1" && rec.value == "v1");

        assert(reader.ReadRecord(&rec, &s) && s.ok());
        assert(rec.seq == 2 && rec.type == lsmkv::ValueType::kTypeValue && rec.key == "k2" && rec.value == "v2");

        assert(reader.ReadRecord(&rec, &s) && s.ok());
        assert(rec.seq == 3 && rec.type == lsmkv::ValueType::kTypeDeletion && rec.key == "k1");

        assert(!reader.ReadRecord(&rec, &s) && s.ok());
    }

    ::unlink(path.c_str());
}

struct Register {
    Register() {
        RegisterTest("WalAppendAndRead", &TestWalAppendAndRead);
    }
} reg;

} // namespace
