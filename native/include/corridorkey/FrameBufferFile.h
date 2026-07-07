#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace corridorkey {

struct FrameBufferSpec {
    std::string path;
    int width = 0;
    int height = 0;
    int channels = 4;
    std::string dtype = "float32";
    std::size_t byteOffset = 0;

    std::size_t elementCount() const;
    std::size_t byteSize() const;
};

std::string makeTempFramePath(const std::string& prefix);
void writeFloatFrame(const FrameBufferSpec& spec, const float* data, std::size_t count);
std::vector<float> readFloatFrame(const FrameBufferSpec& spec);
void removeFileIfExists(const std::string& path);

} // namespace corridorkey

