#pragma once

#include "net/Protocol.hpp"

namespace net::codec {

class ByteWriter {
public:
    void writeU8(std::uint8_t value);
    void writeBool(bool value);
    void writeU16(std::uint16_t value);
    void writeU32(std::uint32_t value);
    void writeU64(std::uint64_t value);
    void writeI32(std::int32_t value);
    void writeF32(float value);
    void writeString(const std::string& value);
    ByteBuffer take() &&;

private:
    ByteBuffer bytes_{};
};

class ByteReader {
public:
    explicit ByteReader(const ByteBuffer& bytes);

    bool readU8(std::uint8_t* value);
    bool readBool(bool* value);
    bool readU16(std::uint16_t* value);
    bool readU32(std::uint32_t* value);
    bool readU64(std::uint64_t* value);
    bool readI32(std::int32_t* value);
    bool readF32(float* value);
    bool readString(std::string* value);

    bool atEnd() const;
    std::size_t offset() const;

private:
    bool has(std::size_t bytes) const;

    const ByteBuffer& bytes_;
    std::size_t offset_{0u};
};

}  // namespace net::codec
