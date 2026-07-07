#include "corridorkey/FrameBufferFile.h"
#include "corridorkey/WorkerClient.h"

#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxParam.h>
#include <ofxProperty.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kPluginIdentifier = "com.kw.corridorkey.resolve";
constexpr int kPluginVersionMajor = 0;
constexpr int kPluginVersionMinor = 1;

const OfxHost* gHost = nullptr;
OfxPropertySuiteV1* gProperties = nullptr;
OfxImageEffectSuiteV1* gEffects = nullptr;
OfxParameterSuiteV1* gParams = nullptr;

struct OfxImage {
    OfxPropertySetHandle handle = nullptr;
    void* data = nullptr;
    int bounds[4] = {0, 0, 0, 0};
    int rowBytes = 0;
};

int widthOf(const OfxImage& image)
{
    return image.bounds[2] - image.bounds[0];
}

int heightOf(const OfxImage& image)
{
    return image.bounds[3] - image.bounds[1];
}

void requireSuites()
{
    if (!gHost) {
        throw std::runtime_error("OFX host is not initialized");
    }
    if (!gProperties) {
        gProperties = const_cast<OfxPropertySuiteV1*>(
            reinterpret_cast<const OfxPropertySuiteV1*>(gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1)));
    }
    if (!gEffects) {
        gEffects = const_cast<OfxImageEffectSuiteV1*>(
            reinterpret_cast<const OfxImageEffectSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1)));
    }
    if (!gParams) {
        gParams = const_cast<OfxParameterSuiteV1*>(
            reinterpret_cast<const OfxParameterSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1)));
    }
    if (!gProperties || !gEffects || !gParams) {
        throw std::runtime_error("required OFX suites are unavailable");
    }
}

std::string choiceValue(int index, std::initializer_list<const char*> values)
{
    const auto count = static_cast<int>(values.size());
    index = std::max(0, std::min(index, count - 1));
    return *(values.begin() + index);
}

OfxStatus getImage(OfxImageClipHandle clip, OfxTime time, OfxImage& out)
{
    if (!clip) {
        return kOfxStatErrBadHandle;
    }
    OfxStatus status = gEffects->clipGetImage(clip, time, nullptr, &out.handle);
    if (status != kOfxStatOK || !out.handle) {
        return status;
    }
    gProperties->propGetPointer(out.handle, kOfxImagePropData, 0, &out.data);
    gProperties->propGetIntN(out.handle, kOfxImagePropBounds, 4, out.bounds);
    gProperties->propGetInt(out.handle, kOfxImagePropRowBytes, 0, &out.rowBytes);
    return kOfxStatOK;
}

void releaseImage(OfxImage& image)
{
    if (image.handle) {
        gEffects->clipReleaseImage(image.handle);
        image.handle = nullptr;
    }
}

std::vector<float> copyImageToRgba(const OfxImage& image)
{
    const int width = widthOf(image);
    const int height = heightOf(image);
    std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4);
    auto* base = static_cast<const unsigned char*>(image.data);
    for (int y = 0; y < height; ++y) {
        const auto* src = reinterpret_cast<const float*>(base + static_cast<std::size_t>(y) * image.rowBytes);
        std::copy(src, src + static_cast<std::size_t>(width) * 4, pixels.begin() + static_cast<std::size_t>(y) * width * 4);
    }
    return pixels;
}

void copyRgbaToImage(const std::vector<float>& pixels, const OfxImage& image)
{
    const int width = widthOf(image);
    const int height = heightOf(image);
    auto* base = static_cast<unsigned char*>(image.data);
    for (int y = 0; y < height; ++y) {
        auto* dst = reinterpret_cast<float*>(base + static_cast<std::size_t>(y) * image.rowBytes);
        std::copy(
            pixels.begin() + static_cast<std::size_t>(y) * width * 4,
            pixels.begin() + static_cast<std::size_t>(y + 1) * width * 4,
            dst);
    }
}

int getChoice(OfxParamSetHandle paramSet, const char* name, OfxTime time, int fallback)
{
    OfxParamHandle param = nullptr;
    if (gParams->paramGetHandle(paramSet, name, &param, nullptr) != kOfxStatOK || !param) {
        return fallback;
    }
    int value = fallback;
    gParams->paramGetValueAtTime(param, time, &value);
    return value;
}

int getInt(OfxParamSetHandle paramSet, const char* name, OfxTime time, int fallback)
{
    OfxParamHandle param = nullptr;
    if (gParams->paramGetHandle(paramSet, name, &param, nullptr) != kOfxStatOK || !param) {
        return fallback;
    }
    int value = fallback;
    gParams->paramGetValueAtTime(param, time, &value);
    return value;
}

double getDouble(OfxParamSetHandle paramSet, const char* name, OfxTime time, double fallback)
{
    OfxParamHandle param = nullptr;
    if (gParams->paramGetHandle(paramSet, name, &param, nullptr) != kOfxStatOK || !param) {
        return fallback;
    }
    double value = fallback;
    gParams->paramGetValueAtTime(param, time, &value);
    return value;
}

bool getBool(OfxParamSetHandle paramSet, const char* name, OfxTime time, bool fallback)
{
    return getInt(paramSet, name, time, fallback ? 1 : 0) != 0;
}

corridorkey::WorkerSettings readSettings(OfxImageEffectHandle effect, OfxTime time)
{
    OfxParamSetHandle paramSet = nullptr;
    gEffects->getParamSet(effect, &paramSet);

    corridorkey::WorkerSettings settings;
    settings.screenColor = choiceValue(getChoice(paramSet, "screenColor", time, 0), {"auto", "green", "blue"});
    settings.inputColorspace = choiceValue(getChoice(paramSet, "inputColorspace", time, 0), {"srgb", "linear"});
    settings.outputMode = choiceValue(
        getChoice(paramSet, "outputMode", time, 0),
        {"processed_rgba", "matte", "straight_fg", "checker_comp"});
    settings.despill = getInt(paramSet, "despill", time, 5);
    settings.autoDespeckle = getBool(paramSet, "autoDespeckle", time, true);
    settings.despeckleSize = getInt(paramSet, "despeckleSize", time, 400);
    settings.refiner = getDouble(paramSet, "refiner", time, 1.0);
    settings.inferenceSize = std::stoi(choiceValue(getChoice(paramSet, "inferenceSize", time, 2), {"512", "1024", "2048"}));
    settings.backend = choiceValue(getChoice(paramSet, "backend", time, 0), {"auto", "torch", "mlx"});
    settings.device = choiceValue(getChoice(paramSet, "device", time, 0), {"auto", "cuda", "mps", "cpu", "rocm"});
    return settings;
}

void setClipFormat(OfxPropertySetHandle props)
{
    gProperties->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProperties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
}

void addChoice(OfxParamSetHandle paramSet, const char* name, const char* label, std::initializer_list<const char*> choices, int defaultValue)
{
    OfxPropertySetHandle props = nullptr;
    gParams->paramDefine(paramSet, kOfxParamTypeChoice, name, &props);
    gProperties->propSetString(props, kOfxPropLabel, 0, label);
    int index = 0;
    for (const char* choice : choices) {
        gProperties->propSetString(props, kOfxParamPropChoiceOption, index++, choice);
    }
    gProperties->propSetInt(props, kOfxParamPropDefault, 0, defaultValue);
}

void addInt(OfxParamSetHandle paramSet, const char* name, const char* label, int defaultValue, int minValue, int maxValue)
{
    OfxPropertySetHandle props = nullptr;
    gParams->paramDefine(paramSet, kOfxParamTypeInteger, name, &props);
    gProperties->propSetString(props, kOfxPropLabel, 0, label);
    gProperties->propSetInt(props, kOfxParamPropDefault, 0, defaultValue);
    gProperties->propSetInt(props, kOfxParamPropMin, 0, minValue);
    gProperties->propSetInt(props, kOfxParamPropMax, 0, maxValue);
}

void addBool(OfxParamSetHandle paramSet, const char* name, const char* label, bool defaultValue)
{
    OfxPropertySetHandle props = nullptr;
    gParams->paramDefine(paramSet, kOfxParamTypeBoolean, name, &props);
    gProperties->propSetString(props, kOfxPropLabel, 0, label);
    gProperties->propSetInt(props, kOfxParamPropDefault, 0, defaultValue ? 1 : 0);
}

void addDouble(OfxParamSetHandle paramSet, const char* name, const char* label, double defaultValue, double minValue, double maxValue)
{
    OfxPropertySetHandle props = nullptr;
    gParams->paramDefine(paramSet, kOfxParamTypeDouble, name, &props);
    gProperties->propSetString(props, kOfxPropLabel, 0, label);
    gProperties->propSetDouble(props, kOfxParamPropDefault, 0, defaultValue);
    gProperties->propSetDouble(props, kOfxParamPropMin, 0, minValue);
    gProperties->propSetDouble(props, kOfxParamPropMax, 0, maxValue);
}

OfxStatus describe(OfxImageEffectHandle effect)
{
    requireSuites();
    OfxPropertySetHandle props = nullptr;
    gEffects->getPropertySet(effect, &props);
    gProperties->propSetString(props, kOfxPropLabel, 0, "CorridorKey");
    gProperties->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Keying");
    gProperties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGeneral);
    gProperties->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextFilter);
    gProperties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gProperties->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gProperties->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);
    return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect)
{
    requireSuites();
    OfxPropertySetHandle clipProps = nullptr;
    gEffects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clipProps);
    setClipFormat(clipProps);
    gEffects->clipDefine(effect, "AlphaHint", &clipProps);
    setClipFormat(clipProps);
    gEffects->clipDefine(effect, kOfxImageEffectOutputClipName, &clipProps);
    setClipFormat(clipProps);

    OfxParamSetHandle paramSet = nullptr;
    gEffects->getParamSet(effect, &paramSet);
    addChoice(paramSet, "screenColor", "Screen Color", {"Auto", "Green", "Blue"}, 0);
    addChoice(paramSet, "inputColorspace", "Input Colorspace", {"sRGB", "Linear"}, 0);
    addChoice(paramSet, "outputMode", "Output Mode", {"Processed RGBA", "Matte", "Straight FG", "Checker Comp"}, 0);
    addInt(paramSet, "despill", "Despill", 5, 0, 10);
    addBool(paramSet, "autoDespeckle", "Auto Despeckle", true);
    addInt(paramSet, "despeckleSize", "Despeckle Size", 400, 0, 100000);
    addDouble(paramSet, "refiner", "Refiner", 1.0, 0.01, 10.0);
    addChoice(paramSet, "inferenceSize", "Inference Size", {"512", "1024", "2048"}, 2);
    addChoice(paramSet, "backend", "Backend", {"Auto", "Torch", "MLX"}, 0);
    addChoice(paramSet, "device", "Device", {"Auto", "CUDA", "MPS", "CPU", "ROCm"}, 0);
    return kOfxStatOK;
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    requireSuites();
    OfxTime time = 0.0;
    gProperties->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    OfxImageClipHandle sourceClip = nullptr;
    OfxImageClipHandle alphaClip = nullptr;
    OfxImageClipHandle outputClip = nullptr;
    gEffects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &sourceClip, nullptr);
    gEffects->clipGetHandle(effect, "AlphaHint", &alphaClip, nullptr);
    gEffects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &outputClip, nullptr);

    OfxImage source;
    OfxImage alpha;
    OfxImage output;
    try {
        if (getImage(sourceClip, time, source) != kOfxStatOK || getImage(alphaClip, time, alpha) != kOfxStatOK
            || getImage(outputClip, time, output) != kOfxStatOK) {
            throw std::runtime_error("failed to fetch one or more OFX images");
        }
        if (widthOf(source) != widthOf(output) || heightOf(source) != heightOf(output)) {
            throw std::runtime_error("source and output dimensions differ");
        }

        corridorkey::FrameBufferSpec sourceSpec;
        sourceSpec.path = corridorkey::makeTempFramePath("resolve_source");
        sourceSpec.width = widthOf(source);
        sourceSpec.height = heightOf(source);
        sourceSpec.channels = 4;

        corridorkey::FrameBufferSpec alphaSpec = sourceSpec;
        alphaSpec.path = corridorkey::makeTempFramePath("resolve_alpha");
        alphaSpec.width = widthOf(alpha);
        alphaSpec.height = heightOf(alpha);

        corridorkey::FrameBufferSpec outputSpec = sourceSpec;
        outputSpec.path = corridorkey::makeTempFramePath("resolve_output");

        const auto sourcePixels = copyImageToRgba(source);
        const auto alphaPixels = copyImageToRgba(alpha);
        corridorkey::writeFloatFrame(sourceSpec, sourcePixels.data(), sourcePixels.size());
        corridorkey::writeFloatFrame(alphaSpec, alphaPixels.data(), alphaPixels.size());

        corridorkey::WorkerClient worker;
        worker.start();
        corridorkey::ProcessRequest request;
        request.source = sourceSpec;
        request.alphaHint = alphaSpec;
        request.output = outputSpec;
        request.settings = readSettings(effect, time);
        worker.process(request);
        worker.stop();

        const auto outputPixels = corridorkey::readFloatFrame(outputSpec);
        copyRgbaToImage(outputPixels, output);
        corridorkey::removeFileIfExists(sourceSpec.path);
        corridorkey::removeFileIfExists(alphaSpec.path);
        corridorkey::removeFileIfExists(outputSpec.path);
    } catch (...) {
        releaseImage(source);
        releaseImage(alpha);
        releaseImage(output);
        return kOfxStatFailed;
    }

    releaseImage(source);
    releaseImage(alpha);
    releaseImage(output);
    return kOfxStatOK;
}

OfxStatus pluginMain(const char* action, const void* handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle)
{
    if (std::strcmp(action, kOfxActionLoad) == 0) {
        requireSuites();
        return kOfxStatOK;
    }
    if (std::strcmp(action, kOfxActionDescribe) == 0) {
        return describe(reinterpret_cast<OfxImageEffectHandle>(const_cast<void*>(handle)));
    }
    if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
        return describeInContext(reinterpret_cast<OfxImageEffectHandle>(const_cast<void*>(handle)));
    }
    if (std::strcmp(action, kOfxImageEffectActionRender) == 0) {
        return render(reinterpret_cast<OfxImageEffectHandle>(const_cast<void*>(handle)), inArgs);
    }
    return kOfxStatReplyDefault;
}

OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi,
    kOfxImageEffectPluginApiVersion,
    kPluginIdentifier,
    kPluginVersionMajor,
    kPluginVersionMinor,
    nullptr,
    pluginMain,
};

} // namespace

extern "C" {

OfxExport int OfxGetNumberOfPlugins()
{
    return 1;
}

OfxExport OfxPlugin* OfxGetPlugin(int nth)
{
    return nth == 0 ? &gPlugin : nullptr;
}

OfxExport OfxStatus OfxSetHost(const OfxHost* host)
{
    gHost = host;
    return kOfxStatOK;
}

}
