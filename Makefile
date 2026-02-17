# Makefile for Vulkan Engine (MinGW-w64)

# Paths
VULKAN_SDK_PATH = C:/VulkanSDK/1.4.341.1

# Compiler and Flags
CXX = g++
CFLAGS = -std=c++20 -O2 -Wall -Wextra -I$(VULKAN_SDK_PATH)/Include -I./src -I./src/vendor -I./src/vendor/imgui -I./src/vendor/imgui/backends
LDFLAGS = -L$(VULKAN_SDK_PATH)/Lib -lvulkan-1 -lgdi32 -luser32 -lkernel32 -ldwmapi

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

# Helper to find all .cpp files recursively (excludes vendor/imgui — handled separately)
rwildcard=$(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))

# Engine source files (exclude vendor directory entirely)
ENGINE_SOURCES := $(call rwildcard,$(SRC_DIR)/core/,*.cpp) \
                  $(call rwildcard,$(SRC_DIR)/gfx/,*.cpp) \
                  $(call rwildcard,$(SRC_DIR)/scene/,*.cpp) \
                  $(call rwildcard,$(SRC_DIR)/ui/,*.cpp) \
                  $(call rwildcard,$(SRC_DIR)/world/,*.cpp) \
                  $(SRC_DIR)/main.cpp

# ImGui sources — only the files we actually use
IMGUI_DIR = $(SRC_DIR)/vendor/imgui
IMGUI_SOURCES := $(IMGUI_DIR)/imgui.cpp \
                 $(IMGUI_DIR)/imgui_draw.cpp \
                 $(IMGUI_DIR)/imgui_tables.cpp \
                 $(IMGUI_DIR)/imgui_widgets.cpp \
                 $(IMGUI_DIR)/backends/imgui_impl_win32.cpp \
                 $(IMGUI_DIR)/backends/imgui_impl_vulkan.cpp

SOURCES := $(ENGINE_SOURCES) $(IMGUI_SOURCES)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

# Executable
TARGET = $(BIN_DIR)/engine.exe

# Shader Compiler
GLSLC = $(VULKAN_SDK_PATH)/Bin/glslc.exe
SHADER_SRC_DIR = shaders
SHADER_BIN_DIR = $(BIN_DIR)/shaders
SHADERS := $(wildcard $(SHADER_SRC_DIR)/*.vert) $(wildcard $(SHADER_SRC_DIR)/*.frag)
SPV_SHADERS := $(SHADERS:$(SHADER_SRC_DIR)/%=$(SHADER_BIN_DIR)/%.spv)

# Targets
all: $(TARGET) shaders

$(TARGET): $(OBJECTS)
	@if not exist "$(BIN_DIR)" mkdir "$(BIN_DIR)"
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@if not exist "$(dir $@)" mkdir "$(dir $@)"
	$(CXX) $(CFLAGS) -c $< -o $@

shaders: $(SPV_SHADERS)

$(SHADER_BIN_DIR)/%.spv: $(SHADER_SRC_DIR)/%
	@if not exist "$(SHADER_BIN_DIR)" mkdir "$(SHADER_BIN_DIR)"
	$(GLSLC) $< -o $@

clean:
	@if exist "$(OBJ_DIR)" rmdir /s /q "$(OBJ_DIR)"
	@if exist "$(BIN_DIR)" rmdir /s /q "$(BIN_DIR)"

.PHONY: all clean shaders
