#include "bloom.h"

namespace lsmkv {

// MurmurHash2 32-bit implementation
uint32_t BloomHash(std::string_view data) {
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    const uint32_t seed = 0xbc9f1d34;
    uint32_t h = seed ^ static_cast<uint32_t>(data.size());

    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
    size_t len = data.size();

    while (len >= 4) {
        uint32_t k = static_cast<uint32_t>(p[0]) |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) |
                    (static_cast<uint32_t>(p[3]) << 24);

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        p += 4;
        len -= 4;
    }

    switch (len) {
        case 3: h ^= static_cast<uint32_t>(p[2]) << 16; [[fallthrough]];
        case 2: h ^= static_cast<uint32_t>(p[1]) << 8;  [[fallthrough]];
        case 1: h ^= static_cast<uint32_t>(p[0]);
                h *= m;
    };

    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

BloomFilterPolicy::BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // k = ln(2) * bits_per_key =~ 0.69 * bits_per_key
    k_ = static_cast<size_t>(bits_per_key * 0.69);
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
}

void BloomFilterPolicy::CreateFilter(const std::vector<std::string>& keys, std::string* dst) const {
    size_t bits = keys.size() * bits_per_key_;
    if (bits < 64) bits = 64;

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_)); // Save number of probe functions at tail

    char* array = &(*dst)[init_size];
    for (const auto& key : keys) {
        uint32_t h = BloomHash(key);
        const uint32_t delta = (h >> 17) | (h << 15); // Rotate right 17 bits
        for (size_t j = 0; j < k_; ++j) {
            const uint32_t bitpos = h % bits;
            array[bitpos / 8] |= (1 << (bitpos % 8));
            h += delta;
        }
    }
}

bool BloomFilterPolicy::KeyMayMatch(std::string_view key, std::string_view filter) const {
    const size_t len = filter.size();
    if (len < 2) return false;

    const size_t bits = (len - 1) * 8;
    const size_t k = static_cast<uint8_t>(filter[len - 1]);
    if (k > 30) return true; // Corrupt filter, fail open

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; ++j) {
        const uint32_t bitpos = h % bits;
        if ((filter[bitpos / 8] & (1 << (bitpos % 8))) == 0) {
            return false;
        }
        h += delta;
    }
    return true;
}

} // namespace lsmkv
