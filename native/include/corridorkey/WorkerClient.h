#pragma once

#include "corridorkey/FrameBufferFile.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace corridorkey {

struct WorkerSettings {
    std::string screenColor = "auto";
    std::string inputColorspace = "srgb";
    int despill = 5;
    bool autoDespeckle = true;
    int despeckleSize = 400;
    double refiner = 1.0;
    int inferenceSize = 512;
    std::string backend = "auto";
    std::string device = "auto";
    std::string outputMode = "processed_rgba";
    bool tiledInference = false;

    nlohmann::json toJson() const;
};

struct ProcessRequest {
    FrameBufferSpec source;
    FrameBufferSpec alphaHint;
    FrameBufferSpec output;
    WorkerSettings settings;
};

struct ProcessResult {
    std::string outputMode;
    std::string screenColor;
    int elapsedMs = 0;
};

class WorkerError : public std::runtime_error {
public:
    explicit WorkerError(const std::string& message);
};

class WorkerClient {
public:
    WorkerClient();
    ~WorkerClient();

    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    void start(const std::vector<std::string>& command = {});
    void stop();
    bool isRunning() const;

    nlohmann::json hello();
    void configure(const WorkerSettings& settings);
    nlohmann::json preflight(const WorkerSettings& settings);
    ProcessResult process(const ProcessRequest& request);

private:
    nlohmann::json request(const std::string& command, const nlohmann::json& payload);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<std::string> defaultWorkerCommand();

} // namespace corridorkey
