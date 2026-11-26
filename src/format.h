#pragma once

#include "lsmkv/db.h"
#include <string>
#include <string_view>

namespace lsmkv {

using SequenceNumber = uint64_t;
static constexpr SequenceNumber kMaxSequenceNumber = 0x00ffffffffffffffULL; // 56 bits

class InternalKey {
public:
    InternalKey() = default;
    InternalKey(std::string_view user_key, SequenceNumber seq, ValueType type);

    static InternalKey DecodeFrom(std::string_view rep);

    std::string_view user_key() const { return user_key_; }
    SequenceNumber sequence_number() const { return sequence_number_; }
    ValueType value_type() const { return type_; }
    std::string_view rep() const { return rep_; }

    static int Compare(std::string_view a, std::string_view b);

private:
    std::string rep_;
    std::string_view user_key_;
    SequenceNumber sequence_number_ = 0;
    ValueType type_ = ValueType::kTypeValue;
};

// Formats user_key + 8-byte tag (7 bytes seq + 1 byte type)
std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq, ValueType type);
std::string_view ExtractUserKey(std::string_view internal_key);
SequenceNumber ExtractSequenceNumber(std::string_view internal_key);
ValueType ExtractValueType(std::string_view internal_key);

} // namespace lsmkv
