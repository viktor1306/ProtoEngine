#pragma once
#include <filesystem>
#include <optional>
struct GLFWwindow;
namespace proto {
std::optional<std::filesystem::path> sceneFileDialog(GLFWwindow* window, bool save,
                                                     const std::filesystem::path& initialDirectory = {});
std::optional<std::filesystem::path> modelFileDialog(GLFWwindow* window);
std::optional<std::filesystem::path> projectFolderDialog(GLFWwindow* window, bool create);
} // namespace proto
