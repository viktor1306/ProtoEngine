#pragma once
#include <string>
struct GLFWwindow;

namespace proto {
// Smoke tests only. Never logs clipboard contents; restores all copied formats.
// Skips without writing if the current clipboard cannot be preserved completely.
std::string probeClipboard(GLFWwindow* window);
}
