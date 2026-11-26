#include "sstable_builder.h"
#include "coding.h"
#include "crc32.h"
#include "bloom.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace lsmkv {

void BlockHandle::EncodeTo(std::string* dst) const {
    PutVarint64(dst, offset);
    PutVarint64(dst, size);
}

bool BlockHandle::DecodeFrom(std::string_view* input) {
    if (!GetVarint64(input, &offset)) return false;
    if (!GetVarint64(input, &size)) return false;
    return true;
}

void Footer::EncodeTo(std::string* dst) const {
    const size_t orig_size = dst->size();
    index_handle.EncodeTo(dst);
    filter_handle.EncodeTo(dst);
    dst->resize(orig_size + 2 * (10 + 10), 0); // fixed padding for handles
    PutFixed64(dst, kMagic);
}

Status Footer::DecodeFrom(std::string_view input) {
    if (input.size() < 8) {
        return Status::Corruption("Footer too short");
    }
    uint64_t magic = DecodeFixed64(input.data() + input.size() - 8);
    if (magic != kMagic) {
        return Status::Corruption("SSTable magic number mismatch");
    }

    std::string_view handles_part = input.substr(0, input.size() - 8);
    if (!index_handle.DecodeFrom(&handles_part)) {
        return Status::Corruption("Bad index handle in footer");
    }
    if (!filter_handle.DecodeFrom(&handles_part)) {
        return Status::Corruption("Bad filter handle in footer");
    }
    return Status::OK();
}

BlockBuilder::BlockBuilder(size_t /*block_size*/) {
    restarts_.push_back(0);
}

void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    last_key_.clear();
    counter_ = 0;
}

size_t BlockBuilder::CurrentSizeEstimate() const {
    return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
    // Prefix compression every 16 keys
    size_t shared = 0;
    if (counter_ < 16) {
        const size_t min_len = std::min(last_key_.size(), key.size());
        while ((shared < min_len) && (last_key_[shared] == key[shared])) {
            shared++;
        }
    } else {
        restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
        counter_ = 0;
    }
    const size_t non_shared = key.size() - shared;

    PutVarint32(&buffer_, static_cast<uint32_t>(shared));
    PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
    PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));

    buffer_.append(key.data() + shared, non_shared);
    buffer_.append(value.data(), value.size());

    last_key_.assign(key.data(), key.size());
    counter_++;
}

std::string BlockBuilder::Finish() {
    for (uint32_t r : restarts_) {
        PutFixed32(&buffer_, r);
    }
    PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
    return buffer_;
}

SSTableBuilder::SSTableBuilder(const Options& options, const std::string& filename)
    : options_(options),
      filename_(filename),
      data_block_(options.block_size),
      index_block_(options.block_size) {
    fd_ = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

SSTableBuilder::~SSTableBuilder() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void SSTableBuilder::Abandon() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        ::unlink(filename_.c_str());
    }
}

void SSTableBuilder::Add(std::string_view key, std::string_view value) {
    if (num_entries_ == 0) {
        smallest_key_ = std::string(key);
    }
    largest_key_ = std::string(key);
    num_entries_++;

    // Track user keys for Bloom filter
    filter_keys_.push_back(std::string(ExtractUserKey(key)));

    data_block_.Add(key, value);

    if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
        FlushBlock();
    }
}

void SSTableBuilder::FlushBlock() {
    if (data_block_.Empty()) return;

    std::string block_content = data_block_.Finish();

    // Write: [data] [crc32: 4]
    uint32_t crc = crc32(block_content.data(), block_content.size());
    std::string trailer;
    PutFixed32(&trailer, crc);

    BlockHandle handle;
    handle.offset = offset_;
    handle.size = block_content.size();

    ::write(fd_, block_content.data(), block_content.size());
    ::write(fd_, trailer.data(), trailer.size());
    offset_ += block_content.size() + trailer.size();

    // Add entry to index: key >= last key in block, value = serialized BlockHandle
    std::string handle_encoding;
    handle.EncodeTo(&handle_encoding);
    index_block_.Add(largest_key_, handle_encoding);

    data_block_.Reset();
}

Status SSTableBuilder::Finish() {
    if (fd_ < 0) return Status::IOError("SSTableBuilder closed or invalid");

    FlushBlock();

    // Build and write Bloom filter block
    BloomFilterPolicy filter_policy(options_.bloom_bits_per_key);
    std::string filter_content;
    filter_policy.CreateFilter(filter_keys_, &filter_content);

    BlockHandle filter_handle;
    filter_handle.offset = offset_;
    filter_handle.size = filter_content.size();

    uint32_t filter_crc = crc32(filter_content.data(), filter_content.size());
    std::string filter_trailer;
    PutFixed32(&filter_trailer, filter_crc);

    ::write(fd_, filter_content.data(), filter_content.size());
    ::write(fd_, filter_trailer.data(), filter_trailer.size());
    offset_ += filter_content.size() + filter_trailer.size();

    // Write index block
    std::string index_content = index_block_.Finish();
    BlockHandle index_handle;
    index_handle.offset = offset_;
    index_handle.size = index_content.size();

    uint32_t index_crc = crc32(index_content.data(), index_content.size());
    std::string index_trailer;
    PutFixed32(&index_trailer, index_crc);

    ::write(fd_, index_content.data(), index_content.size());
    ::write(fd_, index_trailer.data(), index_trailer.size());
    offset_ += index_content.size() + index_trailer.size();

    // Write footer
    Footer footer;
    footer.index_handle = index_handle;
    footer.filter_handle = filter_handle;
    std::string footer_content;
    footer.EncodeTo(&footer_content);

    ::write(fd_, footer_content.data(), footer_content.size());
    offset_ += footer_content.size();

    ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;

    return Status::OK();
}

} // namespace lsmkv
