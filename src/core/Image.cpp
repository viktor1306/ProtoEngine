#include "core/Image.hpp"
#include "core/Diagnostics.hpp"
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace proto {
namespace {
template<class T> struct Com {
    T* value{};
    ~Com() { if (value) value->Release(); }
    T* operator->() const { return value; }
};
void hr(HRESULT result, const char* action) {
    if (FAILED(result)) throw std::runtime_error(std::string(action) + ": " + std::to_string(result));
}
}
void savePng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const uint8_t> rgba) {
    if (rgba.size() != size_t(width) * height * 4) throw std::runtime_error("Invalid PNG buffer dimensions");
    ensureParent(path);
    Com<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(&factory.value)), "WIC factory");
    Com<IWICStream> stream;
    hr(factory->CreateStream(&stream.value), "WIC stream");
    hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "PNG file");
    Com<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder.value), "PNG encoder");
    hr(encoder->Initialize(stream.value, WICBitmapEncoderNoCache), "PNG encoder init");
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> options;
    hr(encoder->CreateNewFrame(&frame.value, &options.value), "PNG frame");
    hr(frame->Initialize(options.value), "PNG frame init");
    hr(frame->SetSize(width, height), "PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr(frame->SetPixelFormat(&format), "PNG pixel format");
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("Unexpected WIC pixel format");
    std::vector<uint8_t> bgra(rgba.begin(), rgba.end());
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    hr(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data()), "PNG write");
    hr(frame->Commit(), "PNG frame commit");
    hr(encoder->Commit(), "PNG commit");
}
PixelChecks checkTriangle(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, std::span<const float> depth) {
    if (rgba.size() != size_t(width) * height * 4 || depth.size() != size_t(width) * height) throw std::runtime_error("Invalid GPU readback dimensions");
    const auto index = [=](float x, float y) {
        const auto px = std::clamp(static_cast<int>((x * 0.5f + 0.5f) * static_cast<float>(width)), 0, static_cast<int>(width) - 1);
        const auto py = std::clamp(static_cast<int>((0.5f - y * 0.5f) * static_cast<float>(height)), 0, static_cast<int>(height) - 1);
        return size_t(py) * width + static_cast<unsigned>(px);
    };
    const auto color = [&](float x, float y) {
        const auto i = index(x, y) * 4;
        return std::array<int, 3>{rgba[i], rgba[i + 1], rgba[i + 2]};
    };
    const auto top = color(0, 0.5f);
    const auto left = color(-0.5f, -0.5f);
    const auto right = color(0.5f, -0.5f);
    const auto center = color(0, 0);
    const auto back = color(0.76f, 0.7f);
    PixelChecks result;
    result.yUp = top[1] > 200 && top[0] < 50 && top[2] < 50;
    result.frontFace = left[0] > 170 && left[2] < 60 && right[2] > 170 && right[0] < 60;
    result.backFaceCulled = back[0] < 40 && back[1] < 40 && back[2] < 60 && std::abs(depth[index(0.76f, 0.7f)] - 1.0f) < 0.0001f;
    result.depthOcclusion = center[2] > 40 && center[0] < 130 && std::abs(depth[index(0, 0)] - 0.25f) < 0.0001f;
    result.clearDepth = std::abs(depth[index(-0.9f, 0.9f)] - 1.0f) < 0.0001f;
    return result;
}
}
