#include "wal.h"
#include "crc32.h"
#include "coding.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace lsmkv {

WalWriter::WalWriter(const std::string& filename, bool sync)
    : sync_(sync), filename_(filename) {
    fd_ = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

WalWriter::~WalWriter() {
    Close();
}

void WalWriter::Close() {
    if (fd_ >= 0) {
        if (sync_) {
            ::fsync(fd_);
        }
        ::close(fd_);
        fd_ = -1;
    }
}

Status WalWriter::Sync() {
    if (fd_ < 0) return Status::IOError("WAL file closed");
    if (::fsync(fd_) != 0) {
        return Status::IOError("fsync failed: " + std::string(strerror(errno)));
    }
    return Status::OK();
}

Status WalWriter::AddRecord(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value) {
    if (fd_ < 0) return Status::IOError("WAL file not open");

    // Payload: [Type: 1] [Seq: 8] [KeyLen: 4] [Key] [ValLen: 4] [Value]
    std::string payload;
    payload.reserve(1 + 8 + 4 + key.size() + 4 + value.size());
    payload.push_back(static_cast<char>(type));
    PutFixed64(&payload, seq);
    PutFixed32(&payload, static_cast<uint32_t>(key.size()));
    payload.append(key.data(), key.size());
    PutFixed32(&payload, static_cast<uint32_t>(value.size()));
    payload.append(value.data(), value.size());

    uint32_t length = static_cast<uint32_t>(payload.size());
    uint32_t crc = crc32(payload.data(), payload.size());

    std::string header;
    header.reserve(8);
    PutFixed32(&header, crc);
    PutFixed32(&header, length);

    ssize_t w1 = ::write(fd_, header.data(), header.size());
    if (w1 < static_cast<ssize_t>(header.size())) {
        return Status::IOError("Failed to write WAL header");
    }

    ssize_t w2 = ::write(fd_, payload.data(), payload.size());
    if (w2 < static_cast<ssize_t>(payload.size())) {
        return Status::IOError("Failed to write WAL payload");
    }

    if (sync_) {
        return Sync();
    }
    return Status::OK();
}

WalReader::WalReader(const std::string& filename) : filename_(filename) {
    fd_ = ::open(filename.c_str(), O_RDONLY);
}

WalReader::~WalReader() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool WalReader::ReadRecord(Record* record, Status* status) {
    if (fd_ < 0) {
        *status = Status::IOError("Cannot open WAL file");
        return false;
    }

    char header_buf[8];
    ssize_t bytes_read = 0;
    while (bytes_read < 8) {
        ssize_t r = ::read(fd_, header_buf + bytes_read, 8 - bytes_read);
        if (r == 0) {
            // Clean EOF at record boundary
            if (bytes_read == 0) {
                *status = Status::OK();
                return false;
            }
            // Torn write: incomplete header
            *status = Status::Corruption("Torn write: incomplete WAL header");
            return false;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            *status = Status::IOError(strerror(errno));
            return false;
        }
        bytes_read += r;
    }

    uint32_t expected_crc = DecodeFixed32(header_buf);
    uint32_t length = DecodeFixed32(header_buf + 4);

    std::string payload;
    payload.resize(length);
    bytes_read = 0;
    while (bytes_read < length) {
        ssize_t r = ::read(fd_, &payload[bytes_read], length - bytes_read);
        if (r == 0) {
            *status = Status::Corruption("Torn write: incomplete WAL record payload");
            return false;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            *status = Status::IOError(strerror(errno));
            return false;
        }
        bytes_read += r;
    }

    uint32_t actual_crc = crc32(payload.data(), payload.size());
    if (actual_crc != expected_crc) {
        *status = Status::Corruption("WAL record checksum mismatch");
        return false;
    }

    std::string_view slice(payload);
    if (slice.size() < 1 + 8 + 4) {
        *status = Status::Corruption("WAL record payload too short");
        return false;
    }

    record->type = static_cast<ValueType>(slice[0]);
    slice.remove_prefix(1);

    record->seq = DecodeFixed64(slice.data());
    slice.remove_prefix(8);

    uint32_t key_len = DecodeFixed32(slice.data());
    slice.remove_prefix(4);
    if (slice.size() < key_len + 4) {
        *status = Status::Corruption("WAL record key length overflow");
        return false;
    }
    record->key = std::string(slice.substr(0, key_len));
    slice.remove_prefix(key_len);

    uint32_t val_len = DecodeFixed32(slice.data());
    slice.remove_prefix(4);
    if (slice.size() < val_len) {
        *status = Status::Corruption("WAL record value length overflow");
        return false;
    }
    record->value = std::string(slice.substr(0, val_len));

    *status = Status::OK();
    return true;
}

} // namespace lsmkv
