#include "format.h"
#include "coding.h"
#include <cassert>

namespace lsmkv {

std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
    std::string result;
    result.reserve(user_key.size() + 8);
    result.append(user_key.data(), user_key.size());
    uint64_t tag = (seq << 8) | static_cast<uint8_t>(type);
    PutFixed64(&result, tag);
    return result;
}

std::string_view ExtractUserKey(std::string_view internal_key) {
    assert(internal_key.size() >= 8);
    return internal_key.substr(0, internal_key.size() - 8);
}

SequenceNumber ExtractSequenceNumber(std::string_view internal_key) {
    assert(internal_key.size() >= 8);
    uint64_t tag = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
    return tag >> 8;
}

ValueType ExtractValueType(std::string_view internal_key) {
    assert(internal_key.size() >= 8);
    uint64_t tag = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
    return static_cast<ValueType>(tag & 0xff);
}

InternalKey::InternalKey(std::string_view user_key, SequenceNumber seq, ValueType type)
    : rep_(MakeInternalKey(user_key, seq, type)),
      user_key_(ExtractUserKey(rep_)),
      sequence_number_(seq),
      type_(type) {}

InternalKey InternalKey::DecodeFrom(std::string_view rep) {
    InternalKey key;
    key.rep_ = std::string(rep);
    if (key.rep_.size() >= 8) {
        key.user_key_ = ExtractUserKey(key.rep_);
        key.sequence_number_ = ExtractSequenceNumber(key.rep_);
        key.type_ = ExtractValueType(key.rep_);
    }
    return key;
}

int InternalKey::Compare(std::string_view a, std::string_view b) {
    std::string_view ukey_a = ExtractUserKey(a);
    std::string_view ukey_b = ExtractUserKey(b);
    int cmp = ukey_a.compare(ukey_b);
    if (cmp != 0) return cmp;

    // Same user key: sort newer sequence numbers first (descending order)
    SequenceNumber seq_a = ExtractSequenceNumber(a);
    SequenceNumber seq_b = ExtractSequenceNumber(b);
    if (seq_a > seq_b) return -1;
    if (seq_a < seq_b) return 1;
    return 0;
}

} // namespace lsmkv
