#include "Settings.h"

#include "../support/Win32Util.h"

namespace ac3spdif {
namespace Settings {

static const wchar_t* kKeyPath = L"Software\\ac3spdif";

static std::string readString(const wchar_t* name, const std::string& fallback) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return fallback;
    }
    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return fallback;
    return narrow(buffer);
}

static void writeString(const wchar_t* name, const std::string& value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const std::wstring wide = widen(value);
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(wide.c_str()),
                   static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

static int readInt(const wchar_t* name, int fallback) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return fallback;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) return fallback;
    return static_cast<int>(value);
}

static void writeInt(const wchar_t* name, int value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD dword = static_cast<DWORD>(value);
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&dword), sizeof(dword));
    RegCloseKey(key);
}

std::string inputDevice() { return readString(L"inputDevice", ""); }
void setInputDevice(const std::string& value) { writeString(L"inputDevice", value); }

std::string outputDevice() { return readString(L"outputDevice", ""); }
void setOutputDevice(const std::string& value) { writeString(L"outputDevice", value); }

int channelCount() { return readInt(L"channelCount", 0); }
void setChannelCount(int value) { writeInt(L"channelCount", value); }

std::string bitrate() { return readString(L"bitrate", ""); }
void setBitrate(const std::string& value) { writeString(L"bitrate", value); }

OutputCodec codec() {
    OutputCodec value = OutputCodec::ac3;
    codec::parse(readString(L"codec", "ac3"), value);
    return value;
}
void setCodec(OutputCodec value) {
    writeString(L"codec", codec::shortName(value));
}

CarrierMode carrier() {
    CarrierMode value = CarrierMode::encoded;
    parseCarrier(readString(L"carrier", "iec61937"), value);
    return value;
}
void setCarrier(CarrierMode value) {
    writeString(L"carrier", carrierName(value));
}

double sampleRate() {
    const int value = readInt(L"sampleRate", 48000);
    return value > 0 ? value : 48000;
}
void setSampleRate(double value) {
    writeInt(L"sampleRate", static_cast<int>(value));
}

int bufferMs() {
    const int stored = readInt(L"bufferMs", Config().bufferMs);
    return stored > 0 ? stored : Config().bufferMs;
}
void setBufferMs(int value) { writeInt(L"bufferMs", value); }

bool selfTest() { return readInt(L"selfTest", 0) != 0; }
void setSelfTest(bool value) { writeInt(L"selfTest", value ? 1 : 0); }

bool autoStartOnLaunch() { return readInt(L"autoStartOnLaunch", 0) != 0; }
void setAutoStartOnLaunch(bool value) {
    writeInt(L"autoStartOnLaunch", value ? 1 : 0);
}

Config makeConfig() {
    Config config;
    config.inputQuery = inputDevice();
    config.outputQuery = outputDevice();
    config.channelCount = channelCount();
    // Leave empty so each codec gets its own default unless overridden.
    config.bitrate = bitrate();
    config.codec = codec();
    config.carrier = carrier();
    config.sampleRate = sampleRate();
    config.bufferMs = bufferMs();
    config.selfTest = selfTest();
    config.statusInterval = 0;  // the UI polls; no printed status line
    config.quiet = true;
    return config;
}

}  // namespace Settings
}  // namespace ac3spdif
