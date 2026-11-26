#include "coding.h"
#include <cstring>

namespace lsmkv {

void PutFixed32(std::string* dst, uint32_t value) {
    char buf[sizeof(value)];
    std::memcpy(buf, &value, sizeof(value));
    dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
    char buf[sizeof(value)];
    std::memcpy(buf, &value, sizeof(value));
    dst->append(buf, sizeof(buf));
}

uint32_t DecodeFixed32(const char* ptr) {
    uint32_t result;
    std::memcpy(&result, ptr, sizeof(result));
    return result;
}

uint64_t DecodeFixed64(const char* ptr) {
    uint64_t result;
    std::memcpy(&result, ptr, sizeof(result));
    return result;
}

void PutVarint32(std::string* dst, uint32_t v) {
    char buf[5];
    char* ptr = buf;
    while (v >= 128) {
        *ptr++ = static_cast<char>((v & 0x7f) | 0x80);
        v >>= 7;
    }
    *ptr++ = static_cast<char>(v);
    dst->append(buf, ptr - buf);
}

void PutVarint64(std::string* dst, uint64_t v) {
    char buf[10];
    char* ptr = buf;
    while (v >= 128) {
        *ptr++ = static_cast<char>((v & 0x7f) | 0x80);
        v >>= 7;
    }
    *ptr++ = static_cast<char>(v);
    dst->append(buf, ptr - buf);
}

bool GetVarint32(std::string_view* input, uint32_t* value) {
    uint32_t result = 0;
    int shift = 0;
    while (!input->empty() && shift <= 28) {
        uint8_t byte = static_cast<uint8_t>((*input)[0]);
        input->remove_prefix(1);
        result |= static_cast<uint32_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool GetVarint64(std::string_view* input, uint64_t* value) {
    uint64_t result = 0;
    int shift = 0;
    while (!input->empty() && shift <= 63) {
        uint8_t byte = static_cast<uint8_t>((*input)[0]);
        input->remove_prefix(1);
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

void PutLengthPrefixedSlice(std::string* dst, std::string_view value) {
    PutVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(std::string_view* input, std::string_view* result) {
    uint32_t len = 0;
    if (!GetVarint32(input, &len)) return false;
    if (input->size() < len) return false;
    *result = input->substr(0, len);
    input->remove_prefix(len);
    return true;
}

} // namespace lsmkv
