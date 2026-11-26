#include "sstable_builder.h"
#include "sstable_reader.h"
#include "bloom.h"
#include <cassert>
#include <unistd.h>
#include <functional>

void RegisterTest(const std::string& name, std::function<void()> fn);

namespace {

void TestSSTableBuildAndRead() {
    std::string path = "/tmp/test_table.sst";
    ::unlink(path.c_str());

    lsmkv::Options options;
    options.block_size = 512;
    options.bloom_bits_per_key = 10;

    {
        lsmkv::SSTableBuilder builder(options, path);
        for (int i = 0; i < 500; ++i) {
            char kbuf[32], vbuf[64];
            snprintf(kbuf, sizeof(kbuf), "key%05d", i);
            snprintf(vbuf, sizeof(vbuf), "value%05d", i);
            std::string ikey = lsmkv::MakeInternalKey(kbuf, i + 1, lsmkv::ValueType::kTypeValue);
            builder.Add(ikey, vbuf);
        }
        assert(builder.Finish().ok());
    }

    {
        std::shared_ptr<lsmkv::SSTableReader> reader;
        assert(lsmkv::SSTableReader::Open(options, path, 0, &reader).ok());

        assert(reader->KeyMayMatch("key00010"));
        assert(reader->KeyMayMatch("key00450"));
        assert(!reader->KeyMayMatch("non_existing_key_99999"));

        std::string val;
        lsmkv::Status read_status;
        std::string target_ikey = lsmkv::MakeInternalKey("key00042", 43, lsmkv::ValueType::kTypeValue);
        lsmkv::Status s = reader->Get(target_ikey, &val, &read_status);
        assert(s.ok());
        assert(val == "value00042");

        auto iter = reader->NewIterator();
        iter->seek_to_first();
        int count = 0;
        while (iter->valid()) {
            count++;
            iter->next();
        }
        assert(count == 500);
    }

    ::unlink(path.c_str());
}

struct Register {
    Register() {
        RegisterTest("SSTableBuildAndRead", &TestSSTableBuildAndRead);
    }
} reg;

} // namespace
