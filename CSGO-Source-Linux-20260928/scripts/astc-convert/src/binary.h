#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

inline uint16_t LE16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t LE32(const uint8_t* p) {
    return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
inline bool RangeFits(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}
