#pragma once
#include <yyjson.h>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace proto {
using Json = yyjson_val;
std::vector<uint8_t> assetBytes(const std::filesystem::path& file, size_t limit = 256 * 1024 * 1024);
std::string sha256(std::span<const uint8_t> bytes);
inline std::string sha256(const std::string& s) {
    return sha256(std::span(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}
std::filesystem::path assetUri(const std::filesystem::path& root, const std::string& uri);
// Resolve an external URI relative to a model directory while constraining
// the canonical result to an explicit project/content boundary.  The legacy
// assetUri overload remains package-local for external imports.
std::filesystem::path assetUriWithin(const std::filesystem::path& base, const std::string& uri,
                                     const std::filesystem::path& boundary);
std::filesystem::path utf8Path(const std::string& value);
std::filesystem::path resourcePath(const std::filesystem::path& root, const std::string& relative);
std::vector<uint8_t> dataUri(const std::string& uri);
struct JsonDoc {
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc{nullptr, yyjson_doc_free};
    explicit JsonDoc(const std::string& bytes);
    Json* root() const { return yyjson_doc_get_root(doc.get()); }
};
Json* get(Json* object, const char* key);
std::string str(Json* value);
double num(Json* value);
} // namespace proto
