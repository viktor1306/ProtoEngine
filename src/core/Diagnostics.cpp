#include "core/Diagnostics.hpp"
#include <windows.h>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace proto {
void ensureParent(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
}
Diagnostics::Diagnostics(const std::filesystem::path& file) {
    ensureParent(file);
    file_.open(file, std::ios::trunc);
    if (!file_) throw std::runtime_error("Cannot open diagnostics log");
}
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
CpuMarker::~CpuMarker() {
    metric_.lastMs = milliseconds(start_);
    metric_.totalMs += metric_.lastMs;
    ++metric_.samples;
}
void Diagnostics::write(std::string_view level, std::string_view message) {
    std::scoped_lock lock(mutex_);
    file_ << std::fixed << std::setprecision(3) << milliseconds(start_) << " ms [" << level << "] " << message << '\n';
    file_.flush();
    std::cerr << '[' << level << "] " << message << '\n';
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid UTF-16 argument");
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
bool validUtf8(std::string_view text) {
    return text.empty() || (text.size() <= INT_MAX && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) != 0);
}
std::string jsonString(std::string_view text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        if (c == '\"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count == buffer.size()) throw std::runtime_error("Cannot resolve executable path");
    std::wstring path(buffer.data(), count);
    // Keep public paths in drive/UNC form. An extended launch path otherwise
    // makes CRT path-component iteration inspect the namespace prefix itself.
    if (path.starts_with(L"\\\\?\\UNC\\"))
        path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\"))
        path.erase(0, 4);
    return std::filesystem::path(path).parent_path();
}
std::filesystem::path nativeFilePath(const std::filesystem::path& path) {
    if (path.native().starts_with(L"\\\\?\\"))
        return path;
    const auto full = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
    if (full.starts_with(L"\\\\?\\"))
        return full;
    if (full.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}
std::filesystem::path windowsFont() {
    wchar_t directory[MAX_PATH]{};
    if (!GetWindowsDirectoryW(directory, MAX_PATH)) throw std::runtime_error("Cannot resolve Windows font directory");
    return std::filesystem::path(directory) / "Fonts" / "segoeui.ttf";
}
void setVulkanValidationPath(const std::filesystem::path& directory) {
    // Some Windows loader builds cannot discover a layer through a Unicode
    // environment search path. Use the existing short alias when available;
    // the directory and pinned layer files remain unchanged.
    auto searchPath = std::filesystem::absolute(directory).lexically_normal();
    const auto size = GetShortPathNameW(searchPath.c_str(), nullptr, 0);
    if (size) {
        std::wstring shortPath(size, L'\0');
        const auto written = GetShortPathNameW(searchPath.c_str(), shortPath.data(), size);
        if (written && written < size) {
            shortPath.resize(written);
            searchPath = std::move(shortPath);
        }
    }
    if (!SetEnvironmentVariableW(L"VK_LAYER_PATH", searchPath.c_str()))
        throw std::runtime_error("Cannot set process-local Vulkan validation path");
}
}
