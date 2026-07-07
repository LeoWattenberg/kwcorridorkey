#include "corridorkey/FrameBufferFile.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace corridorkey {
namespace {

std::filesystem::path tempDirectory()
{
    return std::filesystem::temp_directory_path() / "kwcorridorkey";
}

} // namespace

std::size_t FrameBufferSpec::elementCount() const
{
    if (width <= 0 || height <= 0 || channels <= 0) {
        throw std::invalid_argument("invalid frame dimensions");
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels);
}

std::size_t FrameBufferSpec::byteSize() const
{
    if (dtype != "float32") {
        throw std::invalid_argument("only float32 frame buffers are supported");
    }
    return elementCount() * sizeof(float);
}

std::string makeTempFramePath(const std::string& prefix)
{
    std::filesystem::create_directories(tempDirectory());
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    const auto name = prefix + "_" + std::to_string(now) + "_" + std::to_string(rd()) + ".raw";
    return (tempDirectory() / name).string();
}

void writeFloatFrame(const FrameBufferSpec& spec, const float* data, std::size_t count)
{
    if (count != spec.elementCount()) {
        throw std::invalid_argument("frame element count does not match frame spec");
    }
    std::filesystem::create_directories(std::filesystem::path(spec.path).parent_path());
    std::ofstream out(spec.path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open frame buffer for write: " + spec.path);
    }
    if (spec.byteOffset > 0) {
        std::vector<char> padding(spec.byteOffset, 0);
        out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(spec.byteSize()));
    if (!out) {
        throw std::runtime_error("failed to write frame buffer: " + spec.path);
    }
}

std::vector<float> readFloatFrame(const FrameBufferSpec& spec)
{
    std::ifstream in(spec.path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open frame buffer for read: " + spec.path);
    }
    in.seekg(static_cast<std::streamoff>(spec.byteOffset), std::ios::beg);
    std::vector<float> data(spec.elementCount());
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(spec.byteSize()));
    if (!in) {
        throw std::runtime_error("failed to read frame buffer: " + spec.path);
    }
    return data;
}

void removeFileIfExists(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace corridorkey

