# Makefile for Vulkan Engine (MinGW-w64)

# Paths
VULKAN_SDK_PATH = C:/VulkanSDK/1.4.341.1

# Compiler and Flags
CXX = g++
CFLAGS = -std=c++20 -O2 -Wall -Wextra -I$(VULKAN_SDK_PATH)/Include -I./src -I./src/vendor
LDFLAGS = -L$(VULKAN_SDK_PATH)/Lib -lvulkan-1 -lgdi32 -luser32 -lkernel32

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

# Helper to find all .cpp files recursively
rwildcard=$(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))

# Source Files
SOURCES := $(call rwildcard,$(SRC_DIR)/,*.cpp)
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
