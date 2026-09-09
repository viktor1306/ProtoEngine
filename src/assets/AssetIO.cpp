#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <array>
#include <cmath>
#include <stdexcept>

namespace proto {
std::vector<uint8_t> assetBytes(const std::filesystem::path& file, size_t limit) {
    std::ifstream in(nativeFilePath(file), std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Не знайдено ресурс: " + utf8(file.wstring()));
    const auto n = in.tellg();
    if (n < 0 || static_cast<uint64_t>(n) > limit)
        throw std::runtime_error("Ресурс перевищує ліміт розміру");
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!in)
        throw std::runtime_error("Неповне читання ресурсу");
    return bytes;
}
std::string sha256(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<uint8_t, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider failed");
    struct Clean {
        BCRYPT_ALG_HANDLE& a;
        BCRYPT_HASH_HANDLE& h;
        ~Clean() {
            if (h)
                BCryptDestroyHash(h);
            if (a)
                BCryptCloseAlgorithmProvider(a, 0);
        }
    } clean{alg, hash};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), 32, 0) < 0)
        throw std::runtime_error("SHA256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto b : digest) {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return result;
}
std::filesystem::path utf8Path(const std::string& value) {
    if (!validUtf8(value) || value.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid UTF-8 path");
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::filesystem::path resourcePath(const std::filesystem::path& root, const std::string& relative) {
    if (relative.empty() || relative.find(':') != std::string::npos)
        throw std::runtime_error("Invalid resource path");
    const auto local = utf8Path(relative);
    if (local.is_absolute() || local.has_root_name())
        throw std::runtime_error("Absolute resource path is not allowed");
    const auto base = std::filesystem::weakly_canonical(root), path = std::filesystem::weakly_canonical(base / local);
    const auto within = path.lexically_relative(base);
    if (within.empty() || *within.begin() == ".." || within == ".")
        throw std::runtime_error("Resource path escapes its folder");
    return path;
}
namespace {
std::string decodeAssetUri(const std::string& uri) {
    std::string decoded;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%') {
            if (i + 2 >= uri.size())
                throw std::runtime_error("Invalid URI escape");
            auto digit = [](char c) {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                throw std::runtime_error("Invalid URI escape");
            };
            decoded += static_cast<char>(digit(uri[i + 1]) * 16 + digit(uri[i + 2]));
            i += 2;
        } else
            decoded += uri[i];
    }
    if (decoded.empty() || decoded.find_first_of(":\\\0", 0, 3) != std::string::npos ||
        decoded.find_first_of("?#") != std::string::npos || !validUtf8(decoded))
        throw std::runtime_error("Unsupported external URI");
    return decoded;
}
bool within(const std::filesystem::path& base, const std::filesystem::path& value) {
    const auto relative = value.lexically_relative(base);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}
} // namespace
std::filesystem::path assetUri(const std::filesystem::path& root, const std::string& uri) {
    return resourcePath(root, decodeAssetUri(uri));
}
std::filesystem::path assetUriWithin(const std::filesystem::path& base, const std::string& uri,
                                     const std::filesystem::path& boundary) {
    const auto root = std::filesystem::weakly_canonical(boundary);
    const auto resolved = std::filesystem::weakly_canonical(base / utf8Path(decodeAssetUri(uri)));
    if (!within(root, resolved))
        throw std::runtime_error("Resource URI escapes its project boundary");
    return resolved;
}
std::vector<uint8_t> dataUri(const std::string& uri) {
    const auto comma = uri.find(',');
    if (!uri.starts_with("data:") || comma == std::string::npos ||
        uri.substr(0, comma).find(";base64") == std::string::npos)
        throw std::runtime_error("Only base64 data URIs are supported");
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    uint32_t bits{};
    int count{};
    size_t padding{};
    const auto encodedSize = uri.size() - comma - 1;
    if (!encodedSize || encodedSize % 4 != 0)
        throw std::runtime_error("Malformed base64 length");
    for (size_t i = comma + 1; i < uri.size(); ++i) {
        const char c = uri[i];
        if (c == '=') {
            if (++padding > 2)
                throw std::runtime_error("Malformed base64 padding");
            continue;
        }
        const auto n = alphabet.find(c);
        if (padding || n == std::string_view::npos)
            throw std::runtime_error("Malformed base64 URI");
        bits = (bits << 6) | static_cast<uint32_t>(n);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<uint8_t>(bits >> count));
        }
    }
    if ((padding == 0 && count != 0) || (padding == 1 && count != 2) || (padding == 2 && count != 4) ||
        (bits & ((1u << count) - 1u)))
        throw std::runtime_error("Malformed base64 trailing bits");
    return out;
}
JsonDoc::JsonDoc(const std::string& bytes) {
    yyjson_read_err e{};
    doc.reset(yyjson_read_opts(const_cast<char*>(bytes.data()), bytes.size(), 0, nullptr, &e));
    if (!doc)
        throw std::runtime_error(std::string("Asset JSON: ") + e.msg);
}
Json* get(Json* object, const char* key) {
    auto* v = yyjson_obj_get(object, key);
    if (!v)
        throw std::runtime_error(std::string("Asset field missing: ") + key);
    return v;
}
std::string str(Json* value) {
    if (!yyjson_is_str(value))
        throw std::runtime_error("Expected asset string");
    return {yyjson_get_str(value), yyjson_get_len(value)};
}
double num(Json* value) {
    if (!yyjson_is_num(value) || !std::isfinite(yyjson_get_num(value)))
        throw std::runtime_error("Expected finite asset number");
    return yyjson_get_num(value);
}
} // namespace proto
