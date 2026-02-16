#include "ShaderHotReloader.hpp"
#include <iostream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <algorithm>

// Windows specific for _popen
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#include <windows.h>
#endif

namespace core {

ShaderHotReloader::ShaderHotReloader() : m_running(false), m_pendingReload(false) {
    // Determine compiler path
    // 1. Check if glslc is in PATH (simple check by trying to run it)
    if (system("glslc --version > nul 2>&1") == 0) {
        m_compilerPath = "glslc";
    } else {
        // 2. Fallback to VULKAN_SDK
        const char* sdkPath = std::getenv("VULKAN_SDK");
        if (sdkPath) {
            m_compilerPath = std::string(sdkPath) + "/Bin/glslc.exe";
        } else {
            std::cerr << "[ShaderHotReloader] Warning: VULKAN_SDK not found and glslc not in PATH." << std::endl;
            m_compilerPath = "glslc"; // Hope for the best
        }
    }
    std::cout << "[ShaderHotReloader] Using compiler: " << m_compilerPath << std::endl;
}

ShaderHotReloader::~ShaderHotReloader() {
    stop();
}

void ShaderHotReloader::watch(const std::string& filepath) {
    try {
        if (std::filesystem::exists(filepath)) {
            WatchedFile file;
            file.filepath = filepath;
            file.lastWriteTime = std::filesystem::last_write_time(filepath);
            m_watchedFiles.push_back(file);
            std::cout << "[ShaderHotReloader] Watching: " << filepath << std::endl;
        } else {
            std::cerr << "[ShaderHotReloader] Error: File not found: " << filepath << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[ShaderHotReloader] Error accessing file: " << filepath << " : " << e.what() << std::endl;
    }
}

void ShaderHotReloader::start() {
    if (m_running) return;
    m_running = true;
    m_watcherThread = std::thread(&ShaderHotReloader::watcherLoop, this);
}

void ShaderHotReloader::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_watcherThread.joinable()) {
        m_watcherThread.join();
    }
}

bool ShaderHotReloader::shouldReload() {
    return m_pendingReload.load();
}

void ShaderHotReloader::ackReload() {
    m_pendingReload.store(false);
}

void ShaderHotReloader::watcherLoop() {
    while (m_running) {
        bool changesDetected = false;
        bool compilationSuccess = true;

        for (auto& file : m_watchedFiles) {
            try {
                auto currentWriteTime = std::filesystem::last_write_time(file.filepath);
                if (currentWriteTime > file.lastWriteTime) {
                    // File changed!
                    std::cout << "[ShaderHotReloader] Change detected: " << file.filepath << std::endl;
                    file.lastWriteTime = currentWriteTime;
                    
                    // Small delay to ensure write is complete
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    // Attempt Compilation
                    if (!compileShader(file.filepath)) {
                        compilationSuccess = false;
                    } else {
                         changesDetected = true;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[ShaderHotReloader] File check error: " << e.what() << std::endl;
            }
        }

        if (changesDetected && compilationSuccess) {
            m_pendingReload.store(true);
            std::cout << "[ShaderHotReloader] Shaders recompiled successfully. Requesting Pipeline Reload." << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool ShaderHotReloader::compileShader(const std::string& filepath) {
    std::string outputPath = "bin/" + filepath + ".spv";
    // Ensure directory exists
    std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path());

    std::string command = m_compilerPath + " " + filepath + " -o " + outputPath;
    
    // Capture output
    std::string result = "";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "[ShaderHotReloader] Failed to run glslc!" << std::endl;
        return false;
    }

    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result += buffer;
    }

    int exitCode = pclose(pipe);

    if (exitCode != 0) {
        // Set Red Color for Error
        #ifdef _WIN32
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        #endif
        
        std::cerr << "================ SHADER COMPILATION ERROR ================" << std::endl;
        std::cerr << "File: " << filepath << std::endl;
        std::cerr << result << std::endl;
        std::cerr << "==========================================================" << std::endl;
        
        #ifdef _WIN32
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); // Reset
        #endif
        
        return false;
    }

    #ifdef _WIN32
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "[ShaderHotReloader] Compiled: " << filepath << std::endl;
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); // Reset
    #else
    std::cout << "[ShaderHotReloader] Compiled: " << filepath << std::endl;
    #endif

    return true;
}

} // namespace core
