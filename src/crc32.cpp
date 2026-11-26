#include "crc32.h"

namespace lsmkv {

namespace {

class CRC32Table {
public:
    uint32_t table[256];
    constexpr CRC32Table() : table() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (size_t j = 0; j < 8; j++) {
                if (c & 1) {
                    c = 0xedb88320L ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[i] = c;
        }
    }
};

static constexpr CRC32Table kTable;

} // namespace

uint32_t crc32_extend(uint32_t crc, const void* data, size_t length) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    while (length--) {
        crc = kTable.table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t crc32(const void* data, size_t length) {
    return crc32_extend(0, data, length);
}

} // namespace lsmkv
