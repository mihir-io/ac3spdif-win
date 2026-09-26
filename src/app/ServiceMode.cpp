#include "ServiceMode.h"

#include "../engine/Engine.h"
#include "Settings.h"

namespace ac3spdif {
namespace ServiceMode {

static const wchar_t* kRunKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName = L"ac3spdif";
static const wchar_t* kMutexName = L"Local\\ac3spdif.service";
static const wchar_t* kStopEventName = L"Local\\ac3spdif.service.stop";

static std::wstring executablePath() {
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    return module;
}

bool isInstalled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status =
        RegQueryValueExW(key, kRunValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void install() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        throw Failure("cannot open the Run key to install the service");
    }
    const std::wstring command = L"\"" + executablePath() + L"\" --service";
    const LSTATUS status = RegSetValueExW(
        key, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        throw Failure("cannot write the Run key entry for the service");
    }
}

void uninstall() {
    // Remove the login entry and stop any live instance: "turn the service
    // off" should mean off now, not off after the next reboot.
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_WRITE, &key) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(key, kRunValueName);
        RegCloseKey(key);
    }
    requestStop();
}

bool isRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}

bool requestStop() {
    HANDLE stopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!stopEvent) return !isRunning();
    SetEvent(stopEvent);
    CloseHandle(stopEvent);
    // The service tears the engine down and exits; give it the same grace the
    // engine itself gets.
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (!isRunning()) return true;
        Sleep(200);
    }
    return false;
}

int run() {
    ComInit com;

    // Claiming the mutex is the whole "am I the only one" protocol. It is
    // held, not just checked, for the life of the service.
    Handle mutex(CreateMutexW(nullptr, TRUE, kMutexName));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    Handle stopEvent(CreateEventW(nullptr, TRUE, FALSE, kStopEventName));
    if (!stopEvent) return 1;

    // Restart after failures with a fixed backoff — the device may just be
    // waking from sleep, or the receiver was off. This is launchd KeepAlive,
    // spelled by hand.
    while (WaitForSingleObject(stopEvent.get(), 0) != WAIT_OBJECT_0) {
        Engine engine(Settings::makeConfig());
        engine.start();
        while (isActive(engine.state()) &&
               WaitForSingleObject(stopEvent.get(), 500) != WAIT_OBJECT_0) {
        }
        engine.stop();
        engine.waitUntilStopped(8);
        if (WaitForSingleObject(stopEvent.get(), 0) == WAIT_OBJECT_0) break;
        // Reached on failure (a clean engine never stops by itself).
        if (WaitForSingleObject(stopEvent.get(), 10000) == WAIT_OBJECT_0) break;
    }
    return 0;
}

}  // namespace ServiceMode
}  // namespace ac3spdif
