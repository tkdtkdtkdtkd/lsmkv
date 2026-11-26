#pragma once

#include "lsmkv/db.h"
#include "format.h"
#include <string>
#include <memory>
#include <fstream>

namespace lsmkv {

// WAL Record format:
// [CRC32: 4 bytes] [Length: 4 bytes] [Type: 1 byte] [Seq: 8 bytes] [KeyLength: 4 bytes] [Key] [ValueLength: 4 bytes] [Value]
class WalWriter {
public:
    explicit WalWriter(const std::string& filename, bool sync = false);
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    Status AddRecord(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);
    Status Sync();
    void Close();

private:
    int fd_ = -1;
    bool sync_ = false;
    std::string filename_;
};

class WalReader {
public:
    explicit WalReader(const std::string& filename);
    ~WalReader();

    struct Record {
        SequenceNumber seq;
        ValueType type;
        std::string key;
        std::string value;
    };

    // Reads next valid record. Returns false on EOF or corruption.
    // If corruption is encountered, status is populated.
    bool ReadRecord(Record* record, Status* status);

private:
    int fd_ = -1;
    std::string filename_;
};

} // namespace lsmkv
