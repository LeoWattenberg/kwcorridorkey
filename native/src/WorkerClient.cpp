#include "corridorkey/WorkerClient.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace corridorkey {
namespace {

std::uint32_t nextRequestId()
{
    static std::uint32_t id = 1;
    return id++;
}

std::string quoteCommandPart(const std::string& part)
{
    if (part.find_first_of(" \t\"") == std::string::npos) {
        return part;
    }
    std::string out = "\"";
    for (char ch : part) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out += ch;
        }
    }
    out += "\"";
    return out;
}

nlohmann::json frameSpecToJson(const FrameBufferSpec& spec)
{
    return {
        {"path", spec.path},
        {"width", spec.width},
        {"height", spec.height},
        {"channels", spec.channels},
        {"dtype", spec.dtype},
        {"byte_offset", spec.byteOffset},
    };
}

std::filesystem::path currentModulePath()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&currentModulePath),
            &module)) {
        return {};
    }
    std::string path(MAX_PATH, '\0');
    DWORD size = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
    while (size == path.size()) {
        path.resize(path.size() * 2);
        size = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
    }
    if (size == 0) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path);
#else
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&currentModulePath), &info) == 0 || !info.dli_fname) {
        return {};
    }
    return std::filesystem::path(info.dli_fname);
#endif
}

std::filesystem::path bundleLocalWorkerPython()
{
    const auto modulePath = currentModulePath();
    if (modulePath.empty()) {
        return {};
    }
    const auto platformDir = modulePath.parent_path();
    const auto contentsDir = platformDir.parent_path();
#if defined(_WIN32)
    const auto python = contentsDir / "Resources" / "corridorkey-runtime" / ".venv" / "Scripts" / "python.exe";
#else
    const auto python = contentsDir / "Resources" / "corridorkey-runtime" / ".venv" / "bin" / "python";
#endif
    if (std::filesystem::exists(python)) {
        return python;
    }
    return {};
}

} // namespace

WorkerError::WorkerError(const std::string& message)
    : std::runtime_error(message)
{
}

nlohmann::json WorkerSettings::toJson() const
{
    return {
        {"screen_color", screenColor},
        {"input_colorspace", inputColorspace},
        {"despill", despill},
        {"auto_despeckle", autoDespeckle},
        {"despeckle_size", despeckleSize},
        {"refiner", refiner},
        {"inference_size", inferenceSize},
        {"backend", backend},
        {"device", device},
        {"output_mode", outputMode},
        {"tiled_inference", tiledInference},
    };
}

class WorkerClient::Impl {
public:
    ~Impl()
    {
        stop();
    }

    void start(const std::vector<std::string>& command)
    {
        if (running_) {
            return;
        }
        command_ = command.empty() ? defaultWorkerCommand() : command;
        spawn();
        running_ = true;
    }

    void stop()
    {
        if (!running_) {
            return;
        }
        try {
            sendRequest("shutdown", nlohmann::json::object());
        } catch (...) {
        }
        closePipes();
        waitForChild();
        running_ = false;
    }

    bool isRunning() const
    {
        return running_;
    }

    nlohmann::json sendRequest(const std::string& command, const nlohmann::json& payload)
    {
        if (!running_ && command != "shutdown") {
            throw WorkerError("worker is not running");
        }
        const auto id = nextRequestId();
        nlohmann::json request = {
            {"id", id},
            {"command", command},
            {"payload", payload},
        };
        writeMessage(request);
        auto response = readMessage();
        if (!response.value("ok", false)) {
            const auto error = response.value("error", nlohmann::json::object());
            throw WorkerError(error.value("message", "worker command failed"));
        }
        return response.value("result", nlohmann::json::object());
    }

private:
    void writeMessage(const nlohmann::json& message)
    {
        const std::string payload = message.dump();
        const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
        std::array<unsigned char, 4> header = {
            static_cast<unsigned char>(length & 0xff),
            static_cast<unsigned char>((length >> 8) & 0xff),
            static_cast<unsigned char>((length >> 16) & 0xff),
            static_cast<unsigned char>((length >> 24) & 0xff),
        };
        writeAll(reinterpret_cast<const char*>(header.data()), header.size());
        writeAll(payload.data(), payload.size());
    }

    nlohmann::json readMessage()
    {
        std::array<unsigned char, 4> header {};
        readAll(reinterpret_cast<char*>(header.data()), header.size());
        const std::uint32_t length = static_cast<std::uint32_t>(header[0])
            | (static_cast<std::uint32_t>(header[1]) << 8)
            | (static_cast<std::uint32_t>(header[2]) << 16)
            | (static_cast<std::uint32_t>(header[3]) << 24);
        if (length > 16u * 1024u * 1024u) {
            throw WorkerError("worker response is too large");
        }
        std::string payload(length, '\0');
        readAll(payload.data(), payload.size());
        return nlohmann::json::parse(payload);
    }

#if defined(_WIN32)
    void spawn()
    {
        SECURITY_ATTRIBUTES sa {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE childStdoutRead = nullptr;
        HANDLE childStdoutWrite = nullptr;
        HANDLE childStdinRead = nullptr;
        HANDLE childStdinWrite = nullptr;
        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)
            || !SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0)
            || !CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)
            || !SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
            throw WorkerError("failed to create worker pipes");
        }

        std::ostringstream cmd;
        for (std::size_t i = 0; i < command_.size(); ++i) {
            if (i != 0) {
                cmd << ' ';
            }
            cmd << quoteCommandPart(command_[i]);
        }
        std::string commandLine = cmd.str();

        STARTUPINFOA startup {};
        startup.cb = sizeof(startup);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        startup.hStdOutput = childStdoutWrite;
        startup.hStdInput = childStdinRead;
        startup.dwFlags |= STARTF_USESTDHANDLES;

        PROCESS_INFORMATION process {};
        std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back('\0');
        if (!CreateProcessA(
                nullptr,
                mutableCommand.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process)) {
            throw WorkerError("failed to launch worker process");
        }

        CloseHandle(childStdoutWrite);
        CloseHandle(childStdinRead);
        stdoutRead_ = childStdoutRead;
        stdinWrite_ = childStdinWrite;
        process_ = process.hProcess;
        thread_ = process.hThread;
    }

    void writeAll(const char* data, std::size_t size)
    {
        std::size_t written = 0;
        while (written < size) {
            DWORD chunk = 0;
            if (!WriteFile(stdinWrite_, data + written, static_cast<DWORD>(size - written), &chunk, nullptr)) {
                throw WorkerError("failed to write to worker");
            }
            written += chunk;
        }
    }

    void readAll(char* data, std::size_t size)
    {
        std::size_t read = 0;
        while (read < size) {
            DWORD chunk = 0;
            if (!ReadFile(stdoutRead_, data + read, static_cast<DWORD>(size - read), &chunk, nullptr) || chunk == 0) {
                throw WorkerError("failed to read from worker");
            }
            read += chunk;
        }
    }

    void closePipes()
    {
        if (stdinWrite_) {
            CloseHandle(stdinWrite_);
            stdinWrite_ = nullptr;
        }
        if (stdoutRead_) {
            CloseHandle(stdoutRead_);
            stdoutRead_ = nullptr;
        }
    }

    void waitForChild()
    {
        if (process_) {
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (thread_) {
            CloseHandle(thread_);
            thread_ = nullptr;
        }
    }

    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
#else
    void spawn()
    {
        int stdinPipe[2] {};
        int stdoutPipe[2] {};
        if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0) {
            throw WorkerError("failed to create worker pipes");
        }
        pid_ = fork();
        if (pid_ < 0) {
            throw WorkerError("failed to fork worker");
        }
        if (pid_ == 0) {
            dup2(stdinPipe[0], STDIN_FILENO);
            dup2(stdoutPipe[1], STDOUT_FILENO);
            close(stdinPipe[0]);
            close(stdinPipe[1]);
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);
            std::vector<char*> argv;
            argv.reserve(command_.size() + 1);
            for (auto& part : command_) {
                argv.push_back(part.data());
            }
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(stdinPipe[0]);
        close(stdoutPipe[1]);
        stdinWrite_ = stdinPipe[1];
        stdoutRead_ = stdoutPipe[0];
    }

    void writeAll(const char* data, std::size_t size)
    {
        std::size_t written = 0;
        while (written < size) {
            const auto chunk = write(stdinWrite_, data + written, size - written);
            if (chunk <= 0) {
                throw WorkerError("failed to write to worker");
            }
            written += static_cast<std::size_t>(chunk);
        }
    }

    void readAll(char* data, std::size_t size)
    {
        std::size_t readBytes = 0;
        while (readBytes < size) {
            const auto chunk = read(stdoutRead_, data + readBytes, size - readBytes);
            if (chunk <= 0) {
                throw WorkerError("failed to read from worker");
            }
            readBytes += static_cast<std::size_t>(chunk);
        }
    }

    void closePipes()
    {
        if (stdinWrite_ >= 0) {
            close(stdinWrite_);
            stdinWrite_ = -1;
        }
        if (stdoutRead_ >= 0) {
            close(stdoutRead_);
            stdoutRead_ = -1;
        }
    }

    void waitForChild()
    {
        if (pid_ > 0) {
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    int stdinWrite_ = -1;
    int stdoutRead_ = -1;
    pid_t pid_ = -1;
#endif

    std::vector<std::string> command_;
    bool running_ = false;
};

WorkerClient::WorkerClient()
    : impl_(std::make_unique<Impl>())
{
}

WorkerClient::~WorkerClient() = default;

void WorkerClient::start(const std::vector<std::string>& command)
{
    impl_->start(command);
}

void WorkerClient::stop()
{
    impl_->stop();
}

bool WorkerClient::isRunning() const
{
    return impl_->isRunning();
}

nlohmann::json WorkerClient::hello()
{
    return request("hello", nlohmann::json::object());
}

void WorkerClient::configure(const WorkerSettings& settings)
{
    request("configure", {{"settings", settings.toJson()}});
}

nlohmann::json WorkerClient::preflight(const WorkerSettings& settings)
{
    return request("preflight", {{"settings", settings.toJson()}});
}

ProcessResult WorkerClient::process(const ProcessRequest& processRequest)
{
    const auto result = request(
        "process",
        {
            {"settings", processRequest.settings.toJson()},
            {"source", frameSpecToJson(processRequest.source)},
            {"alpha_hint", frameSpecToJson(processRequest.alphaHint)},
            {"output", frameSpecToJson(processRequest.output)},
        });
    ProcessResult out;
    out.outputMode = result.value("output_mode", "");
    out.screenColor = result.value("screen_color", "");
    out.elapsedMs = result.value("elapsed_ms", 0);
    return out;
}

nlohmann::json WorkerClient::request(const std::string& command, const nlohmann::json& payload)
{
    return impl_->sendRequest(command, payload);
}

std::vector<std::string> defaultWorkerCommand()
{
    const char* configuredPython = std::getenv("CORRIDORKEY_WORKER_PYTHON");
    std::string python;
    if (configuredPython && configuredPython[0]) {
        python = configuredPython;
    } else {
        const auto bundledPython = bundleLocalWorkerPython();
        python = bundledPython.empty() ? "python" : bundledPython.string();
    }
    return {python, "-m", "corridorkey_worker", "--stdio"};
}

} // namespace corridorkey
