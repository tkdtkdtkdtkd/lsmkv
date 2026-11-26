#pragma once

#include <cstdint>
#include <cstddef>

namespace lsmkv {

uint32_t crc32(const void* data, size_t length);
uint32_t crc32_extend(uint32_t crc, const void* data, size_t length);

} // namespace lsmkv
