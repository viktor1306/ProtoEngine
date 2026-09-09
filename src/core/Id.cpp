#include "core/Id.hpp"
#include <windows.h>
#include <objbase.h>
#include <stdexcept>

namespace proto {
Uuid Uuid::create() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("UUID generation failed");
    Uuid result;
    result.bytes = {uint8_t(guid.Data1 >> 24), uint8_t(guid.Data1 >> 16), uint8_t(guid.Data1 >> 8), uint8_t(guid.Data1),
        uint8_t(guid.Data2 >> 8), uint8_t(guid.Data2), uint8_t(guid.Data3 >> 8), uint8_t(guid.Data3)};
    for (size_t i = 0; i < 8; ++i) result.bytes[i + 8] = guid.Data4[i];
    return result;
}
Uuid Uuid::parse(std::string_view text) {
    if (text.size() != 36) throw std::runtime_error("UUID must have 36 canonical characters");
    const auto hex = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return uint8_t(c - '0');
        if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
        throw std::runtime_error("UUID must use lowercase hexadecimal");
    };
    Uuid result; size_t at{};
    for (size_t i = 0; i < 16; ++i) {
        if (at == 8 || at == 13 || at == 18 || at == 23)
            if (text[at++] != '-') throw std::runtime_error("Invalid UUID separators");
        result.bytes[i] = uint8_t(hex(text[at]) * 16 + hex(text[at + 1])); at += 2;
    }
    if (result.empty()) throw std::runtime_error("Nil UUID is not an object/resource ID");
    return result;
}
std::string Uuid::string() const {
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(36);
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
    }
    return result;
}
}
