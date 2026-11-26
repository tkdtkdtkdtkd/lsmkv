#include "sstable_reader.h"
#include "coding.h"
#include "crc32.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cassert>

namespace lsmkv {

Block::Block(std::string data) : data_(std::move(data)) {
    if (data_.size() < sizeof(uint32_t)) {
        num_restarts_ = 0;
        restart_offset_ = 0;
    } else {
        num_restarts_ = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
        restart_offset_ = static_cast<uint32_t>(data_.size() - (1 + num_restarts_) * sizeof(uint32_t));
    }
}

Block::Iterator::Iterator(const Block* block)
    : block_(block),
      restart_offset_(block->restart_offset_),
      num_restarts_(block->num_restarts_) {}

void Block::Iterator::seek_to_first() {
    current_offset_ = 0;
    restart_index_ = 0;
    key_.clear();
    ParseNextEntry();
}

bool Block::Iterator::ParseNextEntry() {
    if (current_offset_ >= restart_offset_) {
        valid_ = false;
        return false;
    }

    std::string_view input(block_->data_.data() + current_offset_, restart_offset_ - current_offset_);
    uint32_t shared = 0, non_shared = 0, value_length = 0;
    if (!GetVarint32(&input, &shared) ||
        !GetVarint32(&input, &non_shared) ||
        !GetVarint32(&input, &value_length) ||
        input.size() < non_shared + value_length ||
        key_.size() < shared) {
        valid_ = false;
        return false;
    }

    key_.resize(shared);
    key_.append(input.data(), non_shared);
    input.remove_prefix(non_shared);
    value_ = input.substr(0, value_length);

    current_offset_ = static_cast<uint32_t>(input.data() + value_length - block_->data_.data());
    valid_ = true;
    return true;
}

void Block::Iterator::next() {
    assert(valid());
    ParseNextEntry();
}

void Block::Iterator::seek(std::string_view target) {
    // Binary search through restart array
    int left = 0;
    int right = static_cast<int>(num_restarts_) - 1;
    while (left < right) {
        int mid = (left + right + 1) / 2;
        uint32_t region_offset = DecodeFixed32(block_->data_.data() + restart_offset_ + mid * sizeof(uint32_t));

        std::string_view input(block_->data_.data() + region_offset, restart_offset_ - region_offset);
        uint32_t shared = 0, non_shared = 0, val_len = 0;
        GetVarint32(&input, &shared);
        GetVarint32(&input, &non_shared);
        GetVarint32(&input, &val_len);

        std::string_view mid_key = input.substr(0, non_shared);
        if (InternalKey::Compare(mid_key, target) < 0) {
            left = mid;
        } else {
            right = mid - 1;
        }
    }

    current_offset_ = DecodeFixed32(block_->data_.data() + restart_offset_ + left * sizeof(uint32_t));
    key_.clear();
    while (ParseNextEntry()) {
        if (InternalKey::Compare(key_, target) >= 0) {
            return;
        }
    }
    valid_ = false;
}

// TwoLevelIterator iterating over index block and underlying data blocks
class TwoLevelIterator : public Iterator {
public:
    TwoLevelIterator(const SSTableReader* table, std::unique_ptr<Block::Iterator> index_iter)
        : table_(table), index_iter_(std::move(index_iter)) {}

    bool valid() const override { return data_iter_ && data_iter_->valid(); }

    void seek_to_first() override {
        index_iter_->seek_to_first();
        InitDataBlock();
        if (data_iter_) data_iter_->seek_to_first();
        SkipToNextUserKey();
    }

    void seek(std::string_view target) override {
        index_iter_->seek(target);
        InitDataBlock();
        if (data_iter_) {
            data_iter_->seek(target);
            SkipToNextUserKey();
        }
    }

    void next() override {
        assert(valid());
        data_iter_->next();
        SkipToNextUserKey();
    }

    std::string_view key() const override {
        assert(valid());
        return data_iter_->key();
    }

    std::string_view value() const override {
        assert(valid());
        return data_iter_->value();
    }

private:
    void InitDataBlock() {
        if (!index_iter_->valid()) {
            data_iter_.reset();
            return;
        }
        std::string_view handle_val = index_iter_->value();
        BlockHandle handle;
        if (!handle.DecodeFrom(&handle_val)) {
            data_iter_.reset();
            return;
        }
        std::unique_ptr<Block> block;
        if (!table_->ReadBlock(handle, &block).ok()) {
            data_iter_.reset();
            return;
        }
        data_block_ = std::move(block);
        data_iter_ = data_block_->NewIterator();
    }

    void SkipToNextUserKey() {
        while (data_iter_ == nullptr || !data_iter_->valid()) {
            if (!index_iter_->valid()) break;
            index_iter_->next();
            InitDataBlock();
            if (data_iter_) data_iter_->seek_to_first();
        }
    }

    const SSTableReader* table_;
    std::unique_ptr<Block::Iterator> index_iter_;
    std::unique_ptr<Block> data_block_;
    std::unique_ptr<Block::Iterator> data_iter_;
};

SSTableReader::SSTableReader(const Options& options, int fd)
    : options_(options), fd_(fd), filter_policy_(options.bloom_bits_per_key) {}

SSTableReader::~SSTableReader() {
    if (fd_ >= 0) ::close(fd_);
}

Status SSTableReader::Open(const Options& options, const std::string& filename, uint64_t file_size, std::shared_ptr<SSTableReader>* reader) {
    int fd = ::open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::IOError("Cannot open SSTable " + filename);
    }

    if (file_size < Footer::kEncodedLength) {
        struct stat st;
        if (::fstat(fd, &st) == 0) {
            file_size = st.st_size;
        }
    }

    if (file_size < Footer::kEncodedLength) {
        ::close(fd);
        return Status::Corruption("SSTable file too small");
    }

    // Read footer
    char footer_buf[Footer::kEncodedLength];
    if (::pread(fd, footer_buf, Footer::kEncodedLength, file_size - Footer::kEncodedLength) != static_cast<ssize_t>(Footer::kEncodedLength)) {
        ::close(fd);
        return Status::IOError("Failed reading SSTable footer");
    }

    Footer footer;
    Status s = footer.DecodeFrom(std::string_view(footer_buf, Footer::kEncodedLength));
    if (!s.ok()) {
        ::close(fd);
        return s;
    }

    std::shared_ptr<SSTableReader> table(new SSTableReader(options, fd));

    // Read index block
    s = table->ReadBlock(footer.index_handle, &table->index_block_);
    if (!s.ok()) return s;

    // Read filter block
    if (footer.filter_handle.size > 0) {
        table->filter_data_.resize(footer.filter_handle.size);
        if (::pread(fd, &table->filter_data_[0], footer.filter_handle.size, footer.filter_handle.offset) != static_cast<ssize_t>(footer.filter_handle.size)) {
            return Status::IOError("Failed to read filter block");
        }
    }

    *reader = std::move(table);
    return Status::OK();
}

Status SSTableReader::ReadBlock(const BlockHandle& handle, std::unique_ptr<Block>* result) const {
    std::string buf;
    buf.resize(handle.size);
    if (::pread(fd_, &buf[0], handle.size, handle.offset) != static_cast<ssize_t>(handle.size)) {
        return Status::IOError("Failed to read block data");
    }

    // Read 4-byte CRC trailer
    char trailer[4];
    if (::pread(fd_, trailer, 4, handle.offset + handle.size) != 4) {
        return Status::Corruption("Failed to read block CRC");
    }

    uint32_t expected_crc = DecodeFixed32(trailer);
    uint32_t actual_crc = crc32(buf.data(), buf.size());
    if (actual_crc != expected_crc) {
        return Status::Corruption("Block CRC checksum mismatch");
    }

    *result = std::make_unique<Block>(std::move(buf));
    return Status::OK();
}

bool SSTableReader::KeyMayMatch(std::string_view user_key) const {
    if (filter_data_.empty()) return true;
    return filter_policy_.KeyMayMatch(user_key, filter_data_);
}

Status SSTableReader::Get(std::string_view internal_key, std::string* value, Status* read_status) {
    std::string_view user_key = ExtractUserKey(internal_key);
    if (!KeyMayMatch(user_key)) {
        return Status::NotFound();
    }

    auto index_iter = index_block_->NewIterator();
    index_iter->seek(internal_key);
    if (!index_iter->valid()) {
        return Status::NotFound();
    }

    BlockHandle handle;
    std::string_view handle_str = index_iter->value();
    if (!handle.DecodeFrom(&handle_str)) {
        return Status::Corruption("Corrupt block handle");
    }

    std::unique_ptr<Block> data_block;
    Status s = ReadBlock(handle, &data_block);
    if (!s.ok()) {
        *read_status = s;
        return s;
    }

    auto data_iter = data_block->NewIterator();
    data_iter->seek(internal_key);
    if (data_iter->valid()) {
        std::string_view found_key = data_iter->key();
        if (ExtractUserKey(found_key) == user_key) {
            ValueType type = ExtractValueType(found_key);
            if (type == ValueType::kTypeValue) {
                if (value) *value = std::string(data_iter->value());
                return Status::OK();
            } else if (type == ValueType::kTypeDeletion) {
                *read_status = Status::NotFound();
                return Status::NotFound();
            }
        }
    }
    return Status::NotFound();
}

std::unique_ptr<Iterator> SSTableReader::NewIterator() {
    return std::make_unique<TwoLevelIterator>(this, index_block_->NewIterator());
}

} // namespace lsmkv
