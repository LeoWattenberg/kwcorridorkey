#include "corridorkey/FrameBufferFile.h"
#include "corridorkey/WorkerClient.h"

#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxMessage.h>
#include <ofxParam.h>
#include <ofxProperty.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr const char* kPluginIdentifier = "com.kw.corridorkey.resolve";
constexpr int kPluginVersionMajor = 0;
constexpr int kPluginVersionMinor = 1;

const OfxHost* gHost = nullptr;
OfxPropertySuiteV1* gProperties = nullptr;
OfxImageEffectSuiteV1* gEffects = nullptr;
OfxParameterSuiteV1* gParams = nullptr;
OfxMessageSuiteV2* gMessagesV2 = nullptr;
OfxMessageSuiteV1* gMessages = nullptr;
std::mutex gWorkerMutex;
std::once_flag gLogRotationOnce;
std::unique_ptr<corridorkey::WorkerClient> gWorker;

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
    if (!gMessagesV2) {
        gMessagesV2 = const_cast<OfxMessageSuiteV2*>(
            reinterpret_cast<const OfxMessageSuiteV2*>(gHost->fetchSuite(gHost->host, kOfxMessageSuite, 2)));
    }
    if (!gMessages) {
        gMessages = const_cast<OfxMessageSuiteV1*>(
            reinterpret_cast<const OfxMessageSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1)));
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

std::vector<float> alphaHintFromSourceAlpha(const std::vector<float>& sourcePixels)
{
    std::vector<float> alpha(sourcePixels.size(), 0.0f);
    for (std::size_t i = 0; i + 3 < sourcePixels.size(); i += 4) {
        const float a = sourcePixels[i + 3];
        alpha[i] = a;
        alpha[i + 1] = a;
        alpha[i + 2] = a;
        alpha[i + 3] = 1.0f;
    }
    return alpha;
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

std::vector<std::filesystem::path> diagnosticLogPaths()
{
    std::error_code ec;
    std::vector<std::filesystem::path> paths;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        paths.push_back(temp / "CorridorKeyResolve.log");
    }
    paths.emplace_back("C:\\tmp\\CorridorKeyResolve.log");
    return paths;
}

void appendToDiagnosticLogs(const std::string& message)
{
    for (const auto& path : diagnosticLogPaths()) {
        std::error_code mkdirError;
        std::filesystem::create_directories(path.parent_path(), mkdirError);
        std::ofstream out(path, std::ios::app);
        if (out) {
            out << message << '\n';
        }
    }
}

void rotateDiagnosticLogsOnce()
{
    std::call_once(gLogRotationOnce, [] {
        for (const auto& path : diagnosticLogPaths()) {
            std::error_code mkdirError;
            std::filesystem::create_directories(path.parent_path(), mkdirError);

            std::error_code existsError;
            if (!std::filesystem::exists(path, existsError) || existsError) {
                continue;
            }

            const auto previous = path.parent_path() / (path.stem().string() + ".previous" + path.extension().string());
            std::error_code removeError;
            std::filesystem::remove(previous, removeError);
            std::error_code renameError;
            std::filesystem::rename(path, previous, renameError);
            if (renameError) {
                std::ofstream truncate(path, std::ios::trunc);
            }
        }
        appendToDiagnosticLogs("CorridorKeyResolve log started");
    });
}

void logDiagnostic(OfxImageEffectHandle effect, const std::string& message)
{
#if defined(_WIN32)
    OutputDebugStringA(("CorridorKeyResolve: " + message + "\n").c_str());
#endif
    if (gMessagesV2 && gMessagesV2->setPersistentMessage && effect) {
        gMessagesV2->setPersistentMessage(effect, kOfxMessageError, "CorridorKeyResolveRenderError", "%s", message.c_str());
    }
    if (gMessages && gMessages->message) {
        gMessages->message(effect, kOfxMessageError, "CorridorKeyResolveRenderError", "%s", message.c_str());
    }

    appendToDiagnosticLogs(message);
}

void appendDiagnostic(const std::string& message)
{
#if defined(_WIN32)
    OutputDebugStringA(("CorridorKeyResolve: " + message + "\n").c_str());
#endif
    appendToDiagnosticLogs(message);
}

void requireStatus(OfxStatus status, const char* operation)
{
    if (status != kOfxStatOK) {
        throw std::runtime_error(operation);
    }
}

void requireHandle(const void* handle, const char* operation)
{
    if (!handle) {
        throw std::runtime_error(operation);
    }
}

corridorkey::WorkerSettings readSettings(OfxImageEffectHandle effect, OfxTime time)
{
    OfxParamSetHandle paramSet = nullptr;
    requireStatus(gEffects->getParamSet(effect, &paramSet), "failed to get parameter set");
    requireHandle(paramSet, "parameter set is null");

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
    settings.inferenceSize = std::stoi(choiceValue(getChoice(paramSet, "inferenceSize", time, 0), {"512", "1024", "2048"}));
    settings.backend = choiceValue(getChoice(paramSet, "backend", time, 0), {"auto", "torch", "mlx"});
    settings.device = choiceValue(getChoice(paramSet, "device", time, 0), {"auto", "cuda", "mps", "cpu", "rocm"});
    return settings;
}

void setClipFormat(OfxPropertySetHandle props)
{
    requireHandle(props, "clip properties are null");
    requireStatus(
        gProperties->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA),
        "failed to set clip components");
}

void addChoice(OfxParamSetHandle paramSet, const char* name, const char* label, std::initializer_list<const char*> choices, int defaultValue)
{
    OfxPropertySetHandle props = nullptr;
    requireStatus(gParams->paramDefine(paramSet, kOfxParamTypeChoice, name, &props), "failed to define choice parameter");
    requireHandle(props, "choice parameter properties are null");
    requireStatus(gProperties->propSetString(props, kOfxPropLabel, 0, label), "failed to set choice label");
    int index = 0;
    for (const char* choice : choices) {
        requireStatus(
            gProperties->propSetString(props, kOfxParamPropChoiceOption, index++, choice),
            "failed to set choice option");
    }
    requireStatus(gProperties->propSetInt(props, kOfxParamPropDefault, 0, defaultValue), "failed to set choice default");
}

void addInt(OfxParamSetHandle paramSet, const char* name, const char* label, int defaultValue, int minValue, int maxValue)
{
    OfxPropertySetHandle props = nullptr;
    requireStatus(gParams->paramDefine(paramSet, kOfxParamTypeInteger, name, &props), "failed to define integer parameter");
    requireHandle(props, "integer parameter properties are null");
    requireStatus(gProperties->propSetString(props, kOfxPropLabel, 0, label), "failed to set integer label");
    requireStatus(gProperties->propSetInt(props, kOfxParamPropDefault, 0, defaultValue), "failed to set integer default");
    requireStatus(gProperties->propSetInt(props, kOfxParamPropMin, 0, minValue), "failed to set integer minimum");
    requireStatus(gProperties->propSetInt(props, kOfxParamPropMax, 0, maxValue), "failed to set integer maximum");
}

void addBool(OfxParamSetHandle paramSet, const char* name, const char* label, bool defaultValue)
{
    OfxPropertySetHandle props = nullptr;
    requireStatus(gParams->paramDefine(paramSet, kOfxParamTypeBoolean, name, &props), "failed to define boolean parameter");
    requireHandle(props, "boolean parameter properties are null");
    requireStatus(gProperties->propSetString(props, kOfxPropLabel, 0, label), "failed to set boolean label");
    requireStatus(gProperties->propSetInt(props, kOfxParamPropDefault, 0, defaultValue ? 1 : 0), "failed to set boolean default");
}

void addDouble(OfxParamSetHandle paramSet, const char* name, const char* label, double defaultValue, double minValue, double maxValue)
{
    OfxPropertySetHandle props = nullptr;
    requireStatus(gParams->paramDefine(paramSet, kOfxParamTypeDouble, name, &props), "failed to define double parameter");
    requireHandle(props, "double parameter properties are null");
    requireStatus(gProperties->propSetString(props, kOfxPropLabel, 0, label), "failed to set double label");
    requireStatus(gProperties->propSetDouble(props, kOfxParamPropDefault, 0, defaultValue), "failed to set double default");
    requireStatus(gProperties->propSetDouble(props, kOfxParamPropMin, 0, minValue), "failed to set double minimum");
    requireStatus(gProperties->propSetDouble(props, kOfxParamPropMax, 0, maxValue), "failed to set double maximum");
}

OfxStatus describe(OfxImageEffectHandle effect)
{
    requireSuites();
    OfxPropertySetHandle props = nullptr;
    requireStatus(gEffects->getPropertySet(effect, &props), "failed to get effect properties");
    requireHandle(props, "effect properties are null");
    requireStatus(gProperties->propSetString(props, kOfxPropLabel, 0, "CorridorKey"), "failed to set label");
    requireStatus(gProperties->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Keying"), "failed to set grouping");
    requireStatus(
        gProperties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGeneral),
        "failed to set supported context");
    requireStatus(
        gProperties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat),
        "failed to set supported pixel depth");
    requireStatus(gProperties->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0), "failed to set single instance");
    requireStatus(
        gProperties->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe),
        "failed to set render thread safety");
    return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect)
{
    requireSuites();
    OfxPropertySetHandle clipProps = nullptr;
    requireStatus(gEffects->clipDefine(effect, kOfxImageEffectOutputClipName, &clipProps), "failed to define output clip");
    setClipFormat(clipProps);
    requireStatus(gEffects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clipProps), "failed to define source clip");
    setClipFormat(clipProps);
    requireStatus(gEffects->clipDefine(effect, "AlphaHint", &clipProps), "failed to define alpha hint clip");
    setClipFormat(clipProps);
    requireStatus(gProperties->propSetInt(clipProps, kOfxImageClipPropOptional, 0, 1), "failed to set alpha hint optional");

    OfxParamSetHandle paramSet = nullptr;
    requireStatus(gEffects->getParamSet(effect, &paramSet), "failed to get parameter set");
    requireHandle(paramSet, "parameter set is null");
    addChoice(paramSet, "screenColor", "Screen Color", {"Auto", "Green", "Blue"}, 0);
    addChoice(paramSet, "inputColorspace", "Input Colorspace", {"sRGB", "Linear"}, 0);
    addChoice(paramSet, "outputMode", "Output Mode", {"Processed RGBA", "Matte", "Straight FG", "Checker Comp"}, 0);
    addInt(paramSet, "despill", "Despill", 5, 0, 10);
    addBool(paramSet, "autoDespeckle", "Auto Despeckle", true);
    addInt(paramSet, "despeckleSize", "Despeckle Size", 400, 0, 100000);
    addDouble(paramSet, "refiner", "Refiner", 1.0, 0.01, 10.0);
    addChoice(paramSet, "inferenceSize", "Inference Size", {"512", "1024", "2048"}, 0);
    addChoice(paramSet, "backend", "Backend", {"Auto", "Torch", "MLX"}, 0);
    addChoice(paramSet, "device", "Device", {"Auto", "CUDA", "MPS", "CPU", "ROCm"}, 0);
    return kOfxStatOK;
}

void resetWorker()
{
    std::lock_guard<std::mutex> lock(gWorkerMutex);
    gWorker.reset();
}

corridorkey::ProcessResult processWithSharedWorker(const corridorkey::ProcessRequest& request, OfxTime time)
{
    std::lock_guard<std::mutex> lock(gWorkerMutex);
    if (!gWorker) {
        appendDiagnostic("starting CorridorKey worker at time " + std::to_string(time));
        gWorker = std::make_unique<corridorkey::WorkerClient>();
        gWorker->start();
        appendDiagnostic("CorridorKey worker started at time " + std::to_string(time));
    }
    try {
        appendDiagnostic(
            "sending CorridorKey process request at time " + std::to_string(time)
            + " size=" + std::to_string(request.source.width) + "x" + std::to_string(request.source.height)
            + " inference=" + std::to_string(request.settings.inferenceSize)
            + " device=" + request.settings.device);
        auto result = gWorker->process(request);
        appendDiagnostic(
            "CorridorKey process completed at time " + std::to_string(time)
            + " elapsed_ms=" + std::to_string(result.elapsedMs));
        return result;
    } catch (...) {
        gWorker.reset();
        throw;
    }
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
        if (getImage(sourceClip, time, source) != kOfxStatOK || getImage(outputClip, time, output) != kOfxStatOK) {
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

        corridorkey::FrameBufferSpec outputSpec = sourceSpec;
        outputSpec.path = corridorkey::makeTempFramePath("resolve_output");

        const auto sourcePixels = copyImageToRgba(source);
        const bool hasAlphaHint = getImage(alphaClip, time, alpha) == kOfxStatOK;
        if (hasAlphaHint && (widthOf(alpha) != widthOf(source) || heightOf(alpha) != heightOf(source))) {
            throw std::runtime_error("alpha hint dimensions differ from source dimensions");
        }
        const auto alphaPixels = hasAlphaHint ? copyImageToRgba(alpha) : alphaHintFromSourceAlpha(sourcePixels);
        corridorkey::writeFloatFrame(sourceSpec, sourcePixels.data(), sourcePixels.size());
        corridorkey::writeFloatFrame(alphaSpec, alphaPixels.data(), alphaPixels.size());

        corridorkey::ProcessRequest request;
        request.source = sourceSpec;
        request.alphaHint = alphaSpec;
        request.output = outputSpec;
        request.settings = readSettings(effect, time);
        processWithSharedWorker(request, time);

        const auto outputPixels = corridorkey::readFloatFrame(outputSpec);
        copyRgbaToImage(outputPixels, output);
        corridorkey::removeFileIfExists(sourceSpec.path);
        corridorkey::removeFileIfExists(alphaSpec.path);
        corridorkey::removeFileIfExists(outputSpec.path);
    } catch (const std::exception& exc) {
        logDiagnostic(effect, std::string("render failed at time ") + std::to_string(time) + ": " + exc.what());
        releaseImage(source);
        releaseImage(alpha);
        releaseImage(output);
        return kOfxStatFailed;
    } catch (...) {
        logDiagnostic(effect, std::string("render failed at time ") + std::to_string(time) + ": unknown exception");
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

void pluginSetHost(OfxHost* host)
{
    gHost = host;
}

OfxStatus pluginMain(const char* action, const void* handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle)
{
    if (!action) {
        return kOfxStatFailed;
    }
    try {
        if (std::strcmp(action, kOfxActionLoad) == 0) {
            rotateDiagnosticLogsOnce();
            return kOfxStatOK;
        }
        if (std::strcmp(action, kOfxActionUnload) == 0) {
            resetWorker();
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
    } catch (...) {
        return kOfxStatFailed;
    }
}

OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi,
    kOfxImageEffectPluginApiVersion,
    kPluginIdentifier,
    kPluginVersionMajor,
    kPluginVersionMinor,
    pluginSetHost,
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
    gHost = const_cast<OfxHost*>(host);
    return kOfxStatOK;
}

}
