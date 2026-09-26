// Small Win32/COM conveniences shared by every target.
//
// Nothing here knows about audio. It exists so the interesting files are not
// littered with UTF-8/UTF-16 conversions and HRESULT plumbing — the Win32 API
// speaks UTF-16 and this project's strings, logs and ffmpeg arguments are all
// UTF-8.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Lean-and-mean strips COM out of windows.h, and everything audio here is COM.
#include <objbase.h>

#include <stdexcept>
#include <string>

namespace ac3spdif {

// The one error type the whole project throws. A human-readable sentence,
// because every failure here ends up in front of a person — the CLI prints it,
// the app puts it in a message box — and an HRESULT on its own sends them
// searching.
struct Failure : std::runtime_error {
    explicit Failure(const std::string& description)
        : std::runtime_error(description) {}
};

inline std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), length);
    return wide;
}

inline std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), length, nullptr, nullptr);
    return utf8;
}

inline std::string lowered(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// Human-readable HRESULT: the system message when there is one, the hex code
// always, because "0x88890008" is what a search engine can actually find.
inline std::string hrDescription(HRESULT hr) {
    char buffer[512] = {};
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(hr), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        buffer, sizeof(buffer) - 1, nullptr);
    std::string message(buffer, length);
    while (!message.empty() &&
           (message.back() == '\r' || message.back() == '\n' ||
            message.back() == ' ' || message.back() == '.')) {
        message.pop_back();
    }
    char code[16];
    wsprintfA(code, "0x%08X", static_cast<unsigned>(hr));
    return message.empty() ? std::string(code) : message + " (" + code + ")";
}

inline void throwIfFailed(HRESULT hr, const std::string& what) {
    if (FAILED(hr)) throw Failure(what + ": " + hrDescription(hr));
}

// RAII HANDLE. The Win32 API has two different "no handle" values and APIs
// disagree about which one they return; both are treated as empty here.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : handle_(normalize(h)) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    void reset(HANDLE h = nullptr) {
        if (handle_) CloseHandle(handle_);
        handle_ = normalize(h);
    }
    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    static HANDLE normalize(HANDLE h) {
        return h == INVALID_HANDLE_VALUE ? nullptr : h;
    }
    HANDLE handle_ = nullptr;
};

// Per-thread COM initialization. Every thread that touches WASAPI needs one,
// including worker threads the engine spins up.
class ComInit {
public:
    explicit ComInit(DWORD model = COINIT_MULTITHREADED) {
        hr_ = CoInitializeEx(nullptr, model);
    }
    ~ComInit() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    HRESULT hr_ = E_FAIL;
};

}  // namespace ac3spdif
