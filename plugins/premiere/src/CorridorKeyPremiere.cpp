#include "corridorkey/FrameBufferFile.h"
#include "corridorkey/WorkerClient.h"

#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "entry.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum ParamId {
    PARAM_INPUT = 0,
    PARAM_ALPHA_LAYER,
    PARAM_SCREEN_COLOR,
    PARAM_INPUT_COLORSPACE,
    PARAM_OUTPUT_MODE,
    PARAM_DESPILL,
    PARAM_AUTO_DESPECKLE,
    PARAM_DESPECKLE_SIZE,
    PARAM_REFINER,
    PARAM_INFERENCE_SIZE,
    PARAM_BACKEND,
    PARAM_DEVICE,
    PARAM_COUNT,
};

PF_Err about(PF_InData*, PF_OutData* outData)
{
    std::strncpy(
        outData->return_msg,
        "CorridorKey\rNative Premiere effect using the shared CorridorKey worker bridge.",
        sizeof(outData->return_msg) - 1);
    return PF_Err_NONE;
}

PF_Err globalSetup(PF_InData*, PF_OutData* outData)
{
    outData->my_version = PF_VERSION(0, 1, 0, 0, 0);
    outData->out_flags = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_PIX_INDEPENDENT;
    outData->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE;
    return PF_Err_NONE;
}

PF_Err paramsSetup(PF_InData*, PF_OutData* outData, PF_ParamDef* params[])
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Alpha Hint", PF_LayerDefault_MYSELF, PARAM_ALPHA_LAYER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Screen Color", 3, 1, "Auto|Green|Blue", PARAM_SCREEN_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Input Colorspace", 2, 1, "sRGB|Linear", PARAM_INPUT_COLORSPACE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Output Mode", 4, 1, "Processed RGBA|Matte|Straight FG|Checker Comp", PARAM_OUTPUT_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Despill", 0, 10, 0, 10, 5, PARAM_DESPILL);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Auto Despeckle", "", TRUE, 0, PARAM_AUTO_DESPECKLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Despeckle Size", 0, 100000, 0, 100000, 400, PARAM_DESPECKLE_SIZE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Refiner", 1, 1000, 1, 1000, 0, 100, 1.0, PF_Precision_TENTHS, 0, 0, PARAM_REFINER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Inference Size", 3, 3, "512|1024|2048", PARAM_INFERENCE_SIZE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Backend", 3, 1, "Auto|Torch|MLX", PARAM_BACKEND);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Device", 5, 1, "Auto|CUDA|MPS|CPU|ROCm", PARAM_DEVICE);

    outData->num_params = PARAM_COUNT;
    return err;
}

std::string choice(int value, std::initializer_list<const char*> choices)
{
    const int count = static_cast<int>(choices.size());
    value = std::max(1, std::min(value, count));
    return *(choices.begin() + value - 1);
}

corridorkey::WorkerSettings readSettings(PF_ParamDef* params[])
{
    corridorkey::WorkerSettings settings;
    settings.screenColor = choice(params[PARAM_SCREEN_COLOR]->u.pd.value, {"auto", "green", "blue"});
    settings.inputColorspace = choice(params[PARAM_INPUT_COLORSPACE]->u.pd.value, {"srgb", "linear"});
    settings.outputMode = choice(params[PARAM_OUTPUT_MODE]->u.pd.value, {"processed_rgba", "matte", "straight_fg", "checker_comp"});
    settings.despill = params[PARAM_DESPILL]->u.sd.value;
    settings.autoDespeckle = params[PARAM_AUTO_DESPECKLE]->u.bd.value != 0;
    settings.despeckleSize = params[PARAM_DESPECKLE_SIZE]->u.sd.value;
    settings.refiner = params[PARAM_REFINER]->u.fs_d.value;
    settings.inferenceSize = std::stoi(choice(params[PARAM_INFERENCE_SIZE]->u.pd.value, {"512", "1024", "2048"}));
    settings.backend = choice(params[PARAM_BACKEND]->u.pd.value, {"auto", "torch", "mlx"});
    settings.device = choice(params[PARAM_DEVICE]->u.pd.value, {"auto", "cuda", "mps", "cpu", "rocm"});
    return settings;
}

template <typename PixelT>
std::vector<float> copyWorldToRgba(const PF_EffectWorld& world)
{
    std::vector<float> pixels(static_cast<std::size_t>(world.width) * world.height * 4);
    for (A_long y = 0; y < world.height; ++y) {
        const auto* row = reinterpret_cast<const PixelT*>(static_cast<const char*>(world.data) + static_cast<std::size_t>(y) * world.rowbytes);
        for (A_long x = 0; x < world.width; ++x) {
            const auto& p = row[x];
            const std::size_t offset = (static_cast<std::size_t>(y) * world.width + x) * 4;
            pixels[offset + 0] = static_cast<float>(p.red) / 32768.0f;
            pixels[offset + 1] = static_cast<float>(p.green) / 32768.0f;
            pixels[offset + 2] = static_cast<float>(p.blue) / 32768.0f;
            pixels[offset + 3] = static_cast<float>(p.alpha) / 32768.0f;
        }
    }
    return pixels;
}

template <typename PixelT>
void copyRgbaToWorld(const std::vector<float>& pixels, PF_EffectWorld& world)
{
    for (A_long y = 0; y < world.height; ++y) {
        auto* row = reinterpret_cast<PixelT*>(static_cast<char*>(world.data) + static_cast<std::size_t>(y) * world.rowbytes);
        for (A_long x = 0; x < world.width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * world.width + x) * 4;
            auto to16 = [](float v) -> A_u_short {
                v = std::max(0.0f, std::min(1.0f, v));
                return static_cast<A_u_short>(v * 32768.0f);
            };
            row[x].red = to16(pixels[offset + 0]);
            row[x].green = to16(pixels[offset + 1]);
            row[x].blue = to16(pixels[offset + 2]);
            row[x].alpha = to16(pixels[offset + 3]);
        }
    }
}

PF_Err render(PF_InData*, PF_OutData*, PF_ParamDef* params[], PF_LayerDef* output)
{
    try {
        const PF_EffectWorld& source = params[PARAM_INPUT]->u.ld;
        const PF_EffectWorld& alpha = params[PARAM_ALPHA_LAYER]->u.ld;
        if (!source.data || !alpha.data || !output->data) {
            return PF_Err_BAD_CALLBACK_PARAM;
        }

        corridorkey::FrameBufferSpec sourceSpec;
        sourceSpec.path = corridorkey::makeTempFramePath("premiere_source");
        sourceSpec.width = source.width;
        sourceSpec.height = source.height;
        sourceSpec.channels = 4;

        corridorkey::FrameBufferSpec alphaSpec = sourceSpec;
        alphaSpec.path = corridorkey::makeTempFramePath("premiere_alpha");
        alphaSpec.width = alpha.width;
        alphaSpec.height = alpha.height;

        corridorkey::FrameBufferSpec outputSpec = sourceSpec;
        outputSpec.path = corridorkey::makeTempFramePath("premiere_output");

        const auto sourcePixels = copyWorldToRgba<PF_Pixel16>(source);
        const auto alphaPixels = copyWorldToRgba<PF_Pixel16>(alpha);
        corridorkey::writeFloatFrame(sourceSpec, sourcePixels.data(), sourcePixels.size());
        corridorkey::writeFloatFrame(alphaSpec, alphaPixels.data(), alphaPixels.size());

        corridorkey::WorkerClient worker;
        worker.start();
        corridorkey::ProcessRequest request;
        request.source = sourceSpec;
        request.alphaHint = alphaSpec;
        request.output = outputSpec;
        request.settings = readSettings(params);
        worker.process(request);
        worker.stop();

        const auto outPixels = corridorkey::readFloatFrame(outputSpec);
        copyRgbaToWorld<PF_Pixel16>(outPixels, *output);
        corridorkey::removeFileIfExists(sourceSpec.path);
        corridorkey::removeFileIfExists(alphaSpec.path);
        corridorkey::removeFileIfExists(outputSpec.path);
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return PF_Err_NONE;
}

} // namespace

extern "C" DllExport PF_Err EntryPointFunc(
    PF_Cmd cmd,
    PF_InData* inData,
    PF_OutData* outData,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void*)
{
    switch (cmd) {
    case PF_Cmd_ABOUT:
        return about(inData, outData);
    case PF_Cmd_GLOBAL_SETUP:
        return globalSetup(inData, outData);
    case PF_Cmd_PARAMS_SETUP:
        return paramsSetup(inData, outData, params);
    case PF_Cmd_RENDER:
        return render(inData, outData, params, output);
    default:
        return PF_Err_NONE;
    }
}

