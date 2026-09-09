#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace proto {
using Clock = std::chrono::steady_clock;
struct CpuMetric {
    std::string_view name;
    double lastMs{};
    double totalMs{};
    uint64_t samples{};
};
class CpuMarker {
public:
    explicit CpuMarker(CpuMetric& metric) : metric_(metric) {}
    ~CpuMarker();
private:
    CpuMetric& metric_;
    Clock::time_point start_{Clock::now()};
};
class Diagnostics {
public:
    explicit Diagnostics(const std::filesystem::path& file);
    void write(std::string_view level, std::string_view message);
    std::atomic_uint validationErrors{0};
    std::atomic_uint validationWarnings{0};
    std::atomic_uint loaderWarnings{0};
private:
    std::ofstream file_;
    std::mutex mutex_;
    Clock::time_point start_{Clock::now()};
};
std::string utf8(std::wstring_view text);
bool validUtf8(std::string_view text);
std::string jsonString(std::string_view text);
std::filesystem::path executableDirectory();
std::filesystem::path nativeFilePath(const std::filesystem::path& path);
void setVulkanValidationPath(const std::filesystem::path& directory);
std::filesystem::path windowsFont();
void ensureParent(const std::filesystem::path& path);
double milliseconds(Clock::time_point start);
}
