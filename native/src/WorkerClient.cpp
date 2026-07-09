#include "corridorkey/WorkerClient.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

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

std::string joinCommand(const std::vector<std::string>& command)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << quoteCommandPart(command[i]);
    }
    return out.str();
}

#if defined(_WIN32)
std::string lastSystemError()
{
    const DWORD error = GetLastError();
    if (error == 0) {
        return {};
    }
    char* buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);
    std::string message = size && buffer ? std::string(buffer, size) : "unknown system error";
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}
#endif

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

std::filesystem::path bundleRuntimeRoot()
{
    const auto modulePath = currentModulePath();
    if (modulePath.empty()) {
        return {};
    }
    const auto platformDir = modulePath.parent_path();
    const auto contentsDir = platformDir.parent_path();
    return contentsDir / "Resources" / "corridorkey-runtime";
}

std::filesystem::path venvPythonFromRuntimeRoot(const std::filesystem::path& runtimeRoot)
{
#if defined(_WIN32)
    return runtimeRoot / ".venv" / "Scripts" / "python.exe";
#else
    return runtimeRoot / ".venv" / "bin" / "python";
#endif
}

std::filesystem::path managedPythonFromRuntimeRoot(const std::filesystem::path& runtimeRoot)
{
    const auto pythonRoot = runtimeRoot / "python";
    if (!std::filesystem::exists(pythonRoot)) {
        return {};
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             pythonRoot,
             std::filesystem::directory_options::skip_permission_denied,
             ec)) {
        if (ec || !entry.is_directory(ec)) {
            continue;
        }
#if defined(_WIN32)
        const auto candidate = entry.path() / "python.exe";
#else
        const auto candidate = entry.path() / "bin" / "python";
#endif
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }

    ec.clear();
    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(
             pythonRoot,
             std::filesystem::directory_options::skip_permission_denied,
             ec);
         !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto path = it->path();
        const auto text = path.string();
        if (text.find("\\Lib\\venv\\") != std::string::npos
            || text.find("/Lib/venv/") != std::string::npos
            || text.find("/lib/venv/") != std::string::npos) {
            continue;
        }
#if defined(_WIN32)
        if (path.filename().string() == "python.exe" && std::filesystem::exists(path.parent_path() / "python310.dll", ec)) {
            return path;
        }
#else
        const auto filename = path.filename().string();
        if (filename == "python3" || filename == "python") {
            return path;
        }
#endif
    }
    return {};
}

std::filesystem::path bundleLocalWorkerPython()
{
    const auto runtimeRoot = bundleRuntimeRoot();
    if (runtimeRoot.empty()) {
        return {};
    }

    // Windows venv launchers contain absolute paths to their base interpreter.
    // Prefer the bundle-local uv-managed interpreter and inject site-packages.
    const auto managedPython = managedPythonFromRuntimeRoot(runtimeRoot);
    if (!managedPython.empty() && std::filesystem::exists(managedPython)) {
        return managedPython;
    }

    const auto python = venvPythonFromRuntimeRoot(runtimeRoot);
    if (std::filesystem::exists(python)) {
        return python;
    }
    return {};
}

std::filesystem::path workerRuntimeRootFromPython(const std::string& python)
{
    std::error_code ec;
    auto path = std::filesystem::absolute(std::filesystem::path(python), ec);
    if (ec) {
        return {};
    }
    for (auto current = path.parent_path(); !current.empty(); current = current.parent_path()) {
        if (current.filename().string() == "corridorkey-runtime") {
            return current;
        }
        if (current.filename().string() == ".venv") {
            return current.parent_path();
        }
        if (current == current.root_path()) {
            break;
        }
    }
    return {};
}

#if defined(_WIN32)
std::filesystem::path makeWorkerStderrPath()
{
    char tempPath[MAX_PATH + 1] {};
    const DWORD length = GetTempPathA(MAX_PATH, tempPath);
    std::filesystem::path root = length > 0 ? std::filesystem::path(tempPath) : std::filesystem::temp_directory_path();
    return root / ("CorridorKeyWorker_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()) + ".stderr.log");
}

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string readTextFileFromOffset(const std::filesystem::path& path, std::uintmax_t& offset)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size <= offset) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::ostringstream out;
    out << in.rdbuf();
    offset = size;
    return out.str();
}

void appendWorkerDiagnostic(const std::string& message)
{
    OutputDebugStringA(("CorridorKeyResolve: " + message + "\n").c_str());
    std::error_code ec;
    std::vector<std::filesystem::path> paths;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        paths.push_back(temp / "CorridorKeyResolve.log");
    }
    paths.emplace_back("C:\\tmp\\CorridorKeyResolve.log");
    for (const auto& path : paths) {
        std::error_code mkdirError;
        std::filesystem::create_directories(path.parent_path(), mkdirError);
        std::ofstream out(path, std::ios::app);
        if (out) {
            out << message << '\n';
        }
    }
}

std::string getEnvironmentVariable(const char* name)
{
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string value(needed, '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return value;
}

void setEnvironmentVariableIfMissing(const char* name, const std::filesystem::path& value)
{
    if (!getEnvironmentVariable(name).empty()) {
        return;
    }
    const auto text = value.string();
    SetEnvironmentVariableA(name, text.c_str());
}

void prependEnvironmentPath(const char* name, const std::filesystem::path& path)
{
    const auto text = path.string();
    std::string current = getEnvironmentVariable(name);
    if (current.find(text) != std::string::npos) {
        return;
    }
    if (!current.empty()) {
        current = text + ";" + current;
    } else {
        current = text;
    }
    SetEnvironmentVariableA(name, current.c_str());
}

void applyWorkerEnvironment(const std::filesystem::path& runtimeRoot)
{
    if (runtimeRoot.empty()) {
        return;
    }
    const auto sitePackages = runtimeRoot / ".venv" / "Lib" / "site-packages";
    if (std::filesystem::exists(sitePackages)) {
        prependEnvironmentPath("PYTHONPATH", sitePackages);
    }
    const auto cacheRoot = runtimeRoot / "cache";
    setEnvironmentVariableIfMissing("CORRIDORKEY_CACHE_DIR", cacheRoot);
    setEnvironmentVariableIfMissing("XDG_CACHE_HOME", cacheRoot / "xdg");
    setEnvironmentVariableIfMissing("HF_HOME", cacheRoot / "huggingface");
    setEnvironmentVariableIfMissing("TORCH_HOME", cacheRoot / "torch");
    setEnvironmentVariableIfMissing("NUMBA_CACHE_DIR", cacheRoot / "numba");
    SetEnvironmentVariableA("HF_HUB_DISABLE_SYMLINKS_WARNING", "1");
}

int workerTimeoutMs()
{
    const auto configured = getEnvironmentVariable("CORRIDORKEY_WORKER_TIMEOUT_SECONDS");
    if (!configured.empty()) {
        try {
            return std::max(1, std::stoi(configured)) * 1000;
        } catch (...) {
        }
    }
    return 300000;
}
#endif

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
        workingDirectory_ = command_.empty() ? std::filesystem::path() : workerRuntimeRootFromPython(command_[0]);
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
            std::string message = error.value("message", "worker command failed");
            const std::string traceback = error.value("traceback", "");
            if (!traceback.empty()) {
                message += "\n" + traceback;
            }
            throw WorkerError(message);
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
            std::ostringstream message;
            message << "worker response is too large: length=" << length << " header=0x";
            for (const auto byte : header) {
                message << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
            }
            throw WorkerError(message.str());
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
        stderrPath_ = makeWorkerStderrPath();
        HANDLE childStderrWrite = CreateFileA(
            stderrPath_.string().c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)
            || !SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0)
            || !CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)
            || !SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0)
            || childStderrWrite == INVALID_HANDLE_VALUE) {
            throw WorkerError("failed to create worker pipes");
        }

        std::string commandLine = joinCommand(command_);
        applyWorkerEnvironment(workingDirectory_);

        STARTUPINFOA startup {};
        startup.cb = sizeof(startup);
        startup.hStdError = childStderrWrite;
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
                workingDirectory_.empty() ? nullptr : workingDirectory_.string().c_str(),
                &startup,
                &process)) {
            CloseHandle(childStderrWrite);
            throw WorkerError("failed to launch worker process: " + commandLine + " (" + lastSystemError() + ")");
        }

        CloseHandle(childStdoutWrite);
        CloseHandle(childStdinRead);
        CloseHandle(childStderrWrite);
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
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::milliseconds(workerTimeoutMs());
        auto lastProgressLog = started;
        std::uintmax_t stderrOffset = 0;
        std::size_t read = 0;
        while (read < size) {
            DWORD available = 0;
            if (!PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr)) {
                throw WorkerError(workerExitMessage("failed to read from worker process"));
            }
            if (available == 0) {
                if (process_) {
                    DWORD exitCode = 0;
                    if (GetExitCodeProcess(process_, &exitCode) && exitCode != STILL_ACTIVE) {
                        throw WorkerError(workerExitMessage("worker process exited before responding"));
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw WorkerError(workerExitMessage("timed out waiting for worker response"));
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - lastProgressLog >= std::chrono::seconds(5)) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
                    appendWorkerDiagnostic(
                        "waiting for worker response for " + std::to_string(elapsed)
                        + "s: " + joinCommand(command_));
                    const auto stderrUpdate = readTextFileFromOffset(stderrPath_, stderrOffset);
                    if (!stderrUpdate.empty()) {
                        appendWorkerDiagnostic("worker stderr update:\n" + stderrUpdate);
                    }
                    lastProgressLog = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            DWORD chunk = 0;
            const auto requested = static_cast<DWORD>(std::min<std::size_t>(size - read, available));
            if (!ReadFile(stdoutRead_, data + read, requested, &chunk, nullptr) || chunk == 0) {
                throw WorkerError(workerExitMessage("failed to read from worker process"));
            }
            read += chunk;
        }
    }

    std::string workerExitMessage(const std::string& prefix)
    {
        std::ostringstream message;
        message << prefix << ": " << joinCommand(command_);
        if (!workingDirectory_.empty()) {
            message << "\nworking directory: " << workingDirectory_.string();
        }
        if (process_) {
            WaitForSingleObject(process_, 1000);
            DWORD exitCode = 0;
            if (GetExitCodeProcess(process_, &exitCode)) {
                if (exitCode == STILL_ACTIVE) {
                    message << "\nworker process is still running";
                } else {
                    message << "\nworker exit code: " << exitCode;
                }
            }
        }
        const std::string stderrText = readTextFile(stderrPath_);
        if (!stderrText.empty()) {
            message << "\nworker stderr:\n" << stderrText;
        } else if (!stderrPath_.empty()) {
            message << "\nworker stderr file was empty: " << stderrPath_.string();
        }
        return message.str();
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
    std::filesystem::path workingDirectory_;
#if defined(_WIN32)
    std::filesystem::path stderrPath_;
#endif
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
