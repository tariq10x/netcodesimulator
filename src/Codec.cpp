#include "net/Codec.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace net::codec {

void ByteWriter::writeU8(std::uint8_t value) {
    bytes_.push_back(value);
}

void ByteWriter::writeBool(bool value) {
    writeU8(value ? 1u : 0u);
}

void ByteWriter::writeU16(std::uint16_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::writeU32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void ByteWriter::writeU64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void ByteWriter::writeI32(std::int32_t value) {
    writeU32(static_cast<std::uint32_t>(value));
}

void ByteWriter::writeF32(float value) {
    std::uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(bits);
}

void ByteWriter::writeString(const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("string exceeds protocol limit");
    }
    writeU16(static_cast<std::uint16_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
}

ByteBuffer ByteWriter::take() && {
    return std::move(bytes_);
}

ByteReader::ByteReader(const ByteBuffer& bytes)
    : bytes_(bytes) {}

bool ByteReader::readU8(std::uint8_t* value) {
    if (!has(sizeof(std::uint8_t))) {
        return false;
    }
    *value = bytes_[offset_++];
    return true;
}

bool ByteReader::readBool(bool* value) {
    std::uint8_t raw = 0u;
    if (!readU8(&raw)) {
        return false;
    }
    *value = raw != 0u;
    return true;
}

bool ByteReader::readU16(std::uint16_t* value) {
    if (!has(sizeof(std::uint16_t))) {
        return false;
    }
    *value = static_cast<std::uint16_t>(bytes_[offset_]) |
             static_cast<std::uint16_t>(bytes_[offset_ + 1] << 8);
    offset_ += sizeof(std::uint16_t);
    return true;
}

bool ByteReader::readU32(std::uint32_t* value) {
    if (!has(sizeof(std::uint32_t))) {
        return false;
    }
    *value = static_cast<std::uint32_t>(bytes_[offset_]) |
             (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8) |
             (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16) |
             (static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24);
    offset_ += sizeof(std::uint32_t);
    return true;
}

bool ByteReader::readU64(std::uint64_t* value) {
    if (!has(sizeof(std::uint64_t))) {
        return false;
    }
    *value = 0u;
    for (int shift = 0; shift < 64; shift += 8) {
        *value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
    }
    return true;
}

bool ByteReader::readI32(std::int32_t* value) {
    std::uint32_t raw = 0u;
    if (!readU32(&raw)) {
        return false;
    }
    *value = static_cast<std::int32_t>(raw);
    return true;
}

bool ByteReader::readF32(float* value) {
    std::uint32_t bits = 0u;
    if (!readU32(&bits)) {
        return false;
    }
    std::memcpy(value, &bits, sizeof(bits));
    return true;
}

bool ByteReader::readString(std::string* value) {
    std::uint16_t size = 0u;
    if (!readU16(&size) || !has(size)) {
        return false;
    }
    value->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
}

bool ByteReader::atEnd() const {
    return offset_ == bytes_.size();
}

std::size_t ByteReader::offset() const {
    return offset_;
}

bool ByteReader::has(std::size_t bytes) const {
    return offset_ + bytes <= bytes_.size();
}

}  // namespace net::codec
