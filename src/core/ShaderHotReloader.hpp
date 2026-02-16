#pragma once

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace core {

class ShaderHotReloader {
public:
    ShaderHotReloader();
    ~ShaderHotReloader();

    // Add a shader file to watch (e.g., "shaders/simple.vert")
    void watch(const std::string& filepath);

    // Start the watcher thread
    void start();

    // Stop the watcher thread
    void stop();

    // Check if a reload is pending (thread-safe)
    bool shouldReload();

    // Acknowledge the reload (reset flag)
    void ackReload();

private:
    void watcherLoop();
    bool compileShader(const std::string& filepath);

    struct WatchedFile {
        std::string filepath;
        std::filesystem::file_time_type lastWriteTime;
    };

    std::vector<WatchedFile> m_watchedFiles;
    std::atomic<bool> m_running;
    std::atomic<bool> m_pendingReload;
    std::thread m_watcherThread;
    
    // Path to glslc compiler
    std::string m_compilerPath;
};

} // namespace core
