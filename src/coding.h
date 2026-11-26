#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace lsmkv {

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

uint32_t DecodeFixed32(const char* ptr);
uint64_t DecodeFixed64(const char* ptr);

void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);

bool GetVarint32(std::string_view* input, uint32_t* value);
bool GetVarint64(std::string_view* input, uint64_t* value);

void PutLengthPrefixedSlice(std::string* dst, std::string_view value);
bool GetLengthPrefixedSlice(std::string_view* input, std::string_view* result);

} // namespace lsmkv
