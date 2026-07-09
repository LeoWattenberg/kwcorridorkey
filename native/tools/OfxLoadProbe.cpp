#include <ofxCore.h>

#include <iostream>
#include <string>

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

#if defined(_WIN32)
std::string lastErrorMessage()
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
    std::string message = size && buffer ? std::string(buffer, size) : "unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: ofx_load_probe <path-to-ofx-binary>\n";
        return 2;
    }

#if defined(_WIN32)
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::cerr << "LoadLibrary failed: " << lastErrorMessage() << "\n";
        return 1;
    }

    auto getNumberOfPlugins = reinterpret_cast<int (*)()>(GetProcAddress(module, "OfxGetNumberOfPlugins"));
    auto getPlugin = reinterpret_cast<OfxPlugin* (*)(int)>(GetProcAddress(module, "OfxGetPlugin"));
    auto setHostExport = reinterpret_cast<OfxStatus (*)(const OfxHost*)>(GetProcAddress(module, "OfxSetHost"));
    if (!getNumberOfPlugins || !getPlugin) {
        std::cerr << "missing required OFX exports\n";
        FreeLibrary(module);
        return 1;
    }

    const int count = getNumberOfPlugins();
    std::cout << "plugin count: " << count << "\n";
    if (count < 1) {
        FreeLibrary(module);
        return 1;
    }

    OfxPlugin* plugin = getPlugin(0);
    if (!plugin) {
        std::cerr << "OfxGetPlugin(0) returned null\n";
        FreeLibrary(module);
        return 1;
    }
    std::cout << "identifier: " << (plugin->pluginIdentifier ? plugin->pluginIdentifier : "<null>") << "\n";
    std::cout << "api: " << (plugin->pluginApi ? plugin->pluginApi : "<null>") << "\n";
    if (!plugin->setHost) {
        std::cerr << "plugin struct setHost is null\n";
        FreeLibrary(module);
        return 1;
    }
    if (!plugin->mainEntry) {
        std::cerr << "plugin struct mainEntry is null\n";
        FreeLibrary(module);
        return 1;
    }

    OfxHost host {};
    plugin->setHost(&host);
    if (setHostExport) {
        const OfxStatus status = setHostExport(&host);
        std::cout << "OfxSetHost export status: " << status << "\n";
    }

    const OfxStatus loadStatus = plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
    std::cout << "OfxActionLoad status: " << loadStatus << "\n";
    FreeLibrary(module);
    return loadStatus == kOfxStatOK ? 0 : 1;
#else
    std::cerr << "ofx_load_probe is currently implemented for Windows only\n";
    return 2;
#endif
}
