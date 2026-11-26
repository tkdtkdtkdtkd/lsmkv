#pragma once

#include <string>
#include <vector>
#include <string_view>
#include <cstdint>

namespace lsmkv {

class BloomFilterPolicy {
public:
    explicit BloomFilterPolicy(int bits_per_key);

    void CreateFilter(const std::vector<std::string>& keys, std::string* dst) const;
    bool KeyMayMatch(std::string_view key, std::string_view filter) const;

private:
    size_t bits_per_key_;
    size_t k_;
};

uint32_t BloomHash(std::string_view data);

} // namespace lsmkv
