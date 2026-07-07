#include "corridorkey/FrameBufferFile.h"
#include "corridorkey/WorkerClient.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

int main()
{
    using namespace corridorkey;

    FrameBufferSpec spec;
    spec.path = makeTempFramePath("native_test");
    spec.width = 2;
    spec.height = 2;
    spec.channels = 4;

    std::vector<float> input = {
        1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
    };
    writeFloatFrame(spec, input.data(), input.size());
    auto output = readFloatFrame(spec);
    removeFileIfExists(spec.path);

    if (output != input) {
        std::cerr << "frame buffer roundtrip failed\n";
        return 1;
    }

    WorkerSettings settings;
    auto json = settings.toJson();
    if (json["output_mode"] != "processed_rgba" || json["screen_color"] != "auto") {
        std::cerr << "settings json failed\n";
        return 1;
    }

    return 0;
}

