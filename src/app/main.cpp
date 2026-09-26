// Tray front end.
//
// The app owns an Engine for as long as it is running and tears it down on
// quit, which is what "runs only while the app is open" means in practice —
// though exclusive mode makes even an unclean exit benign on Windows: the
// endpoint is released with the process, so nothing can be left stuck in a
// passthrough format. Service mode is the same engine run headless from a
// Run-key entry, and the two are mutually exclusive because whichever claims
// the device exclusively first wins.
//
// Engine callbacks arrive on the engine's worker thread and are marshalled to
// the message loop with PostMessage; everything UI-side runs single-threaded.

#include "../engine/Engine.h"
#include "../engine/LatencyAdvice.h"
#include "../engine/Version.h"
#include "ServiceMode.h"
#include "Settings.h"
#include "StatusIcon.h"
#include "StatusWindow.h"

#include <shellapi.h>

#include <memory>
#include <string>
#include <vector>

namespace ac3spdif {

static const UINT WMAPP_TRAY = WM_APP + 1;
static const UINT WMAPP_STATE = WM_APP + 2;
static const UINT WMAPP_LOG = WM_APP + 3;
static const UINT WMAPP_QUIT_READY = WM_APP + 4;

enum MenuID : UINT {
    IDM_TOGGLE = 1,
    IDM_STATUS,
    IDM_SELFTEST,
    IDM_SERVICE,
    IDM_AUTOSTART,
    IDM_QUIT,
    IDM_HEADER,
    IDM_CAPTURE_BASE = 1000,   // + index into captureIds_
    IDM_OUTPUT_BASE = 2000,    // + index into outputIds_; 0 is Automatic
    IDM_CHANNELS_BASE = 3000,  // + index into kChannelChoices
    IDM_CODEC_BASE = 3100,     // + 0 ac3, 1 dts
    IDM_BITRATE_BASE = 3200,   // + index; 0 is Automatic
    IDM_CARRIER_BASE = 3300,   // + 0 encoded, 1 pcm
    IDM_LATENCY_BASE = 3400,   // + bursts
};

static const int kChannelChoices[] = {0, 2, 6, 8};

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    int run() {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = windowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = L"ac3spdif.app";
        RegisterClassW(&windowClass);
        window_ = CreateWindowExW(0, windowClass.lpszClassName, L"ac3spdif",
                                  0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  instance_, this);

        statusWindow_ = std::make_unique<StatusWindow>(instance_);
        addTrayIcon();
        SetTimer(window_, 1, 500, nullptr);

        if (Settings::autoStartOnLaunch()) startEngine();

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        removeTrayIcon();
        return 0;
    }

private:
    // MARK: - Engine control

    void startEngine() {
        if (ServiceMode::isRunning()) {
            const int choice = MessageBoxW(
                nullptr,
                L"The background service already holds the output device, so "
                L"the app cannot also stream to it. Stop the service first?",
                L"Service mode is running",
                MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (choice != IDYES) return;
            ServiceMode::requestStop();
        }

        engine_ = std::make_unique<Engine>(Settings::makeConfig());
        HWND window = window_;
        engine_->onStateChange = [window](EngineState state) {
            PostMessageW(window, WMAPP_STATE, static_cast<WPARAM>(state), 0);
        };
        engine_->onLog = [window](const std::string& line) {
            PostMessageW(window, WMAPP_LOG, 0,
                         reinterpret_cast<LPARAM>(new std::string(line)));
        };
        engine_->start();
    }

    void toggleEngine() {
        if (engine_ && isActive(engine_->state())) engine_->stop();
        else startEngine();
    }

    // Settings are baked into the Config at start, so a change only takes
    // effect on a restart. Doing that silently is friendlier than leaving the
    // menu showing one thing while the stream does another. Message-driven:
    // the stop lands as an idle state change, which starts the new run.
    void restartIfRunning() {
        if (!engine_ || !isActive(engine_->state())) return;
        appendLog("Restarting to apply the new setting…");
        pendingRestart_ = true;
        engine_->stop();
    }

    void presentFailure(const std::string& message) {
        MessageBoxW(nullptr,
                    (widen(message) + L"\n\nThe log in the status window has "
                                      L"the full story.").c_str(),
                    L"Could not start streaming",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }

    void appendLog(const std::string& line) { statusWindow_->append(line); }

    // MARK: - Tray icon

    void addTrayIcon() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = WMAPP_TRAY;
        tray_.hIcon = StatusIcon::create(IconState::stopped);
        wcscpy_s(tray_.szTip, L"ac3spdif — stopped");
        Shell_NotifyIconW(NIM_ADD, &tray_);
    }

    void removeTrayIcon() {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        if (tray_.hIcon) DestroyIcon(tray_.hIcon);
    }

    void updateTrayIcon(EngineState state) {
        IconState iconState;
        switch (state) {
            case EngineState::running: iconState = IconState::streaming; break;
            case EngineState::starting:
            case EngineState::stopping: iconState = IconState::busy; break;
            case EngineState::failed: iconState = IconState::failed; break;
            default: iconState = IconState::stopped; break;
        }
        HICON previous = tray_.hIcon;
        tray_.hIcon = StatusIcon::create(iconState);
        tray_.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
        if (previous) DestroyIcon(previous);
    }

    void refreshTooltip() {
        const EngineStatus status =
            engine_ ? engine_->status() : EngineStatus();
        std::wstring tip;
        if (status.state == EngineState::running) {
            wchar_t buffer[128];
            _snwprintf_s(buffer, _TRUNCATE,
                         L"ac3spdif — %.0f s, margin %.0f ms, %+.0f ppm",
                         status.secondsOnWire, status.marginMs,
                         status.driftPPM);
            tip = buffer;
        } else {
            tip = L"ac3spdif — " + widen(stateLabel(status.state));
        }
        wcscpy_s(tray_.szTip, tip.c_str());
        tray_.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    // MARK: - Menu

    void showMenu() {
        // Rebuilt from scratch on every open, exactly like menuNeedsUpdate:
        // the device list, the checkmarks and the header are all live state.
        HMENU menu = CreatePopupMenu();
        const EngineState state = engine_ ? engine_->state() : EngineState::idle;
        const EngineStatus status =
            engine_ ? engine_->status() : EngineStatus();

        AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_HEADER,
                    headerText(state, status).c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        const bool busy = state == EngineState::starting ||
                          state == EngineState::stopping;
        AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), IDM_TOGGLE,
                    isActive(state) ? L"Stop" : L"Start");
        AppendMenuW(menu, MF_STRING, IDM_STATUS, L"Status Window…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(menu, MF_POPUP,
                    reinterpret_cast<UINT_PTR>(captureMenu()),
                    L"Capture From");
        AppendMenuW(menu, MF_POPUP,
                    reinterpret_cast<UINT_PTR>(outputMenu()),
                    L"Output Device");
        AppendMenuW(menu, MF_POPUP,
                    reinterpret_cast<UINT_PTR>(channelMenu()), L"Channels");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(codecMenu()),
                    L"Codec");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(bitrateMenu()),
                    L"Bitrate");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(carrierMenu()),
                    L"Carrier");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(latencyMenu()),
                    L"Latency");

        AppendMenuW(menu,
                    MF_STRING | (Settings::selfTest() ? MF_CHECKED : 0),
                    IDM_SELFTEST, L"Self-Test Tones");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu,
                    MF_STRING | (ServiceMode::isInstalled() ? MF_CHECKED : 0),
                    IDM_SERVICE, L"Run as Background Service");
        AppendMenuW(
            menu, MF_STRING | (Settings::autoStartOnLaunch() ? MF_CHECKED : 0),
            IDM_AUTOSTART, L"Start Streaming When App Opens");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_HEADER,
                    (L"Version " + widen(kVersion)).c_str());
        AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit ac3spdif");

        // The documented tray-menu ritual: the window must be foreground when
        // the menu opens or it will not dismiss on an outside click.
        SetForegroundWindow(window_);
        POINT cursor;
        GetCursorPos(&cursor);
        TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, window_,
                         nullptr);
        DestroyMenu(menu);
    }

    std::wstring headerText(EngineState state, const EngineStatus& status) {
        if (state != EngineState::running) return widen(stateLabel(state));
        wchar_t buffer[192];
        _snwprintf_s(buffer, _TRUNCATE, L"%s · %s · margin %.0f ms · %+.0f ppm",
                     widen(status.outputName).c_str(),
                     widen(status.carrier).c_str(), status.marginMs,
                     status.driftPPM);
        return buffer;
    }

    HMENU captureMenu() {
        HMENU submenu = CreatePopupMenu();
        captureIds_.clear();
        captureIds_.push_back("");
        AppendMenuW(submenu,
                    MF_STRING |
                        (Settings::inputDevice().empty() ? MF_CHECKED : 0),
                    IDM_CAPTURE_BASE,
                    L"Default output (follows Windows)");
        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        for (const auto& device : devices()) {
            captureIds_.push_back(narrow(device.id));
            const std::wstring title =
                widen(device.name) + L"  (" +
                std::to_wstring(device.mixChannels) + L" ch mix)";
            AppendMenuW(submenu,
                        MF_STRING | (captureIds_.back() ==
                                             Settings::inputDevice()
                                         ? MF_CHECKED : 0),
                        IDM_CAPTURE_BASE + captureIds_.size() - 1,
                        title.c_str());
        }
        return submenu;
    }

    HMENU outputMenu() {
        HMENU submenu = CreatePopupMenu();
        outputIds_.clear();
        outputIds_.push_back("");
        AppendMenuW(submenu,
                    MF_STRING |
                        (Settings::outputDevice().empty() ? MF_CHECKED : 0),
                    IDM_OUTPUT_BASE, L"Automatic (best bitstream device)");
        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        for (const auto& device : devices()) {
            outputIds_.push_back(narrow(device.id));
            // Mark what each device can actually do for the codec in use, so
            // the choice is informed rather than a guess from the name.
            const std::wstring title =
                widen(device.name) + L"  [" +
                widen(AudioDevices::capability(device, Settings::codec())) +
                L"]";
            AppendMenuW(submenu,
                        MF_STRING | (outputIds_.back() ==
                                             Settings::outputDevice()
                                         ? MF_CHECKED : 0),
                        IDM_OUTPUT_BASE + outputIds_.size() - 1, title.c_str());
        }
        return submenu;
    }

    HMENU channelMenu() {
        HMENU submenu = CreatePopupMenu();
        // 8 is offered as a *capture* width, not an output format: AC-3
        // cannot encode 7.1, so the label says what actually reaches the
        // receiver.
        static const wchar_t* labels[] = {
            L"Automatic (match Windows speaker layout)", L"Stereo (2.0)",
            L"Surround (5.1)", L"8-channel source → 5.1"};
        for (int index = 0; index < 4; ++index) {
            AppendMenuW(submenu,
                        MF_STRING | (Settings::channelCount() ==
                                             kChannelChoices[index]
                                         ? MF_CHECKED : 0),
                        IDM_CHANNELS_BASE + index, labels[index]);
        }
        return submenu;
    }

    HMENU codecMenu() {
        HMENU submenu = CreatePopupMenu();
        for (int index = 0; index < 2; ++index) {
            const OutputCodec codec =
                index == 0 ? OutputCodec::ac3 : OutputCodec::dts;
            const int frameMs = static_cast<int>(
                LatencyAdvice::frameMilliseconds(codec, Settings::sampleRate()) +
                0.5);
            const std::wstring title =
                widen(codec::name(codec)) + L"  —  " +
                std::to_wstring(frameMs) + L" ms frame";
            AppendMenuW(submenu,
                        MF_STRING |
                            (Settings::codec() == codec ? MF_CHECKED : 0),
                        IDM_CODEC_BASE + index, title.c_str());
        }
        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(submenu, MF_STRING | MF_GRAYED, IDM_HEADER,
                    L"The frame is the largest fixed delay; DTS's is a third "
                    L"of AC-3's");
        return submenu;
    }

    HMENU bitrateMenu() {
        HMENU submenu = CreatePopupMenu();
        // Per codec: the two sets do not overlap at all, and offering an AC-3
        // rate while DTS is selected would offer a setting that stops the
        // pipeline from starting.
        const OutputCodec codec = Settings::codec();
        const std::string stored = Settings::bitrate();
        // Automatic is also ticked when the stored value belongs to the other
        // codec, since that is what the engine will substitute anyway.
        const bool automatic =
            stored.empty() || !codec::acceptsBitrate(codec, stored);
        AppendMenuW(submenu, MF_STRING | (automatic ? MF_CHECKED : 0),
                    IDM_BITRATE_BASE,
                    (L"Automatic (" +
                     widen(codec::defaultBitrate(codec)) + L")").c_str());
        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        const auto& rates = codec::supportedBitrates(codec);
        for (size_t index = 0; index < rates.size(); ++index) {
            AppendMenuW(submenu,
                        MF_STRING | (stored == rates[index] ? MF_CHECKED : 0),
                        IDM_BITRATE_BASE + index + 1,
                        widen(rates[index]).c_str());
        }
        return submenu;
    }

    HMENU carrierMenu() {
        HMENU submenu = CreatePopupMenu();
        DeviceInfo preview;
        const bool deviceHasEncoded =
            AudioDevices::previewOutput(Settings::outputDevice(),
                                        Settings::sampleRate(),
                                        Settings::codec(), preview) &&
            preview.supportsEncoded(Settings::codec());
        const CarrierMode current = Settings::carrier();
        AppendMenuW(submenu,
                    MF_STRING |
                        (current == CarrierMode::encoded ? MF_CHECKED : 0),
                    IDM_CARRIER_BASE, widen(carrierLabel(CarrierMode::encoded)).c_str());
        std::wstring pcmTitle = widen(carrierLabel(CarrierMode::pcm));
        if (deviceHasEncoded) pcmTitle += L"  — not needed on this device";
        AppendMenuW(submenu,
                    MF_STRING | (current == CarrierMode::pcm ? MF_CHECKED : 0),
                    IDM_CARRIER_BASE + 1, pcmTitle.c_str());
        return submenu;
    }

    // The margin floor for the devices currently selected. It is a property
    // of the hardware — mostly the capture device's period — so ask the
    // devices rather than assuming a number measured on some other machine.
    int latencyFloorMs(OutputCodec codec) {
        if (Settings::selfTest()) return 0;
        const double rate = Settings::sampleRate();
        DeviceInfo input, output;
        const bool haveInput =
            AudioDevices::find(Settings::inputDevice(), rate, input);
        const bool haveOutput = AudioDevices::previewOutput(
            Settings::outputDevice(), rate, codec, output);
        return LatencyAdvice::minimumBufferMs(haveInput ? &input : nullptr,
                                              haveOutput ? &output : nullptr,
                                              codec, rate);
    }

    HMENU latencyMenu() {
        HMENU submenu = CreatePopupMenu();
        // A Win32 menu offers no live slider, so the margin is a list. The
        // slack is PCM now, so these are milliseconds, not the whole-burst
        // steps the old encoded ring could be trimmed in.
        static const int choices[] = {10, 15, 20, 25, 30, 40, 50, 80, 120, 200};
        const int floor = latencyFloorMs(Settings::codec());
        const int current = Settings::bufferMs();
        for (const int ms : choices) {
            std::wstring title = std::to_wstring(ms) + L" ms";
            if (ms == Config().bufferMs) title += L"  (default)";
            if (ms < floor) title += L"  ⚠ below the floor";
            AppendMenuW(submenu, MF_STRING | (current == ms ? MF_CHECKED : 0),
                        IDM_LATENCY_BASE + ms, title.c_str());
        }
        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        const std::wstring floorNote =
            floor > 0 ? L"Floor for these devices: " + std::to_wstring(floor) +
                            L" ms"
                      : std::wstring(L"Self-test: no capture, no floor");
        AppendMenuW(submenu, MF_STRING | MF_GRAYED, IDM_HEADER,
                    floorNote.c_str());
        AppendMenuW(submenu, MF_STRING | MF_GRAYED, IDM_HEADER,
                    L"Margin only; add the frame and ~25 ms of devices");
        return submenu;
    }

    // Enumeration is a handful of COM calls per device; caching it for the
    // lifetime of one menu keeps the submenu builders honest and cheap.
    const std::vector<DeviceInfo>& devices() {
        if (!devicesFresh_) {
            devices_ = AudioDevices::all(Settings::sampleRate());
            devicesFresh_ = true;
        }
        return devices_;
    }

    // MARK: - Menu actions

    void pickCodec(OutputCodec codec) {
        Settings::setCodec(codec);
        // A bitrate chosen for the old codec may not exist for the new one —
        // no AC-3 rate is valid for DTS — and ffmpeg fails to open rather
        // than degrading, so clear it instead of carrying it across.
        const std::string stored = Settings::bitrate();
        if (!stored.empty() && !codec::acceptsBitrate(codec, stored)) {
            appendLog(stored + " is not a " + codec::name(codec) +
                      " bitrate; using " + codec::defaultBitrate(codec) + ".");
            Settings::setBitrate("");
        }
        // The margin is PCM, so the floor barely moves between codecs — only
        // a device period longer than a burst makes it codec-dependent. When
        // it does, raise the setting rather than leave one known to drop out.
        const int floor = latencyFloorMs(codec);
        if (Settings::bufferMs() < floor) {
            appendLog("Raised the buffer from " +
                      std::to_string(Settings::bufferMs()) + " to " +
                      std::to_string(floor) + " ms, the floor for " +
                      codec::name(codec) + " on these devices.");
            Settings::setBufferMs(floor);
        }
        restartIfRunning();
    }

    void pickCarrier(CarrierMode mode) {
        // Choosing the fallback on a device that does not need it is almost
        // always a mistake, and the symptom — a receiver that never locks —
        // does not point at the cause.
        if (mode == CarrierMode::pcm) {
            DeviceInfo preview;
            if (AudioDevices::previewOutput(Settings::outputDevice(),
                                            Settings::sampleRate(),
                                            Settings::codec(), preview) &&
                preview.supportsEncoded(Settings::codec())) {
                const int choice = MessageBoxW(
                    nullptr,
                    L"This device takes the encoded IEC 61937 format "
                    L"directly, which sets the non-audio bit the receiver "
                    L"keys on. The PCM carrier exists for devices that "
                    L"refuse the encoded format; on this one it is strictly "
                    L"worse.\n\nSwitch anyway?",
                    L"This device supports IEC 61937 directly",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                        MB_SETFOREGROUND);
                if (choice != IDYES) return;
            }
        }
        Settings::setCarrier(mode);
        restartIfRunning();
    }

    void toggleService() {
        if (ServiceMode::isInstalled()) {
            ServiceMode::uninstall();
            appendLog("Background service removed.");
            return;
        }
        if (engine_ && isActive(engine_->state())) {
            engine_->stop();
            appendLog("Stopped app streaming so the service can take the "
                      "device.");
        }
        try {
            ServiceMode::install();
            // Start it now too: installed should mean running, not "after
            // the next login".
            wchar_t module[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, module, MAX_PATH);
            std::wstring command = L"\"" + std::wstring(module) +
                                   L"\" --service";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr,
                               FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            appendLog("Background service installed; it also starts at login.");
        } catch (const Failure& failure) {
            presentFailure(failure.what());
        }
    }

    void handleCommand(UINT id) {
        devicesFresh_ = false;
        switch (id) {
            case IDM_TOGGLE: toggleEngine(); return;
            case IDM_STATUS: statusWindow_->show(); return;
            case IDM_SELFTEST:
                Settings::setSelfTest(!Settings::selfTest());
                restartIfRunning();
                return;
            case IDM_SERVICE: toggleService(); return;
            case IDM_AUTOSTART:
                Settings::setAutoStartOnLaunch(!Settings::autoStartOnLaunch());
                return;
            case IDM_QUIT: quit(); return;
        }
        if (id >= IDM_LATENCY_BASE) {
            Settings::setBufferMs(static_cast<int>(id - IDM_LATENCY_BASE));
            restartIfRunning();
        } else if (id >= IDM_CARRIER_BASE) {
            pickCarrier(id == IDM_CARRIER_BASE ? CarrierMode::encoded
                                               : CarrierMode::pcm);
        } else if (id >= IDM_BITRATE_BASE) {
            const size_t index = id - IDM_BITRATE_BASE;
            const auto& rates = codec::supportedBitrates(Settings::codec());
            Settings::setBitrate(index == 0 || index > rates.size()
                                     ? "" : rates[index - 1]);
            restartIfRunning();
        } else if (id >= IDM_CODEC_BASE) {
            pickCodec(id == IDM_CODEC_BASE ? OutputCodec::ac3
                                           : OutputCodec::dts);
        } else if (id >= IDM_CHANNELS_BASE) {
            const int index = id - IDM_CHANNELS_BASE;
            if (index >= 0 && index < 4) {
                Settings::setChannelCount(kChannelChoices[index]);
                restartIfRunning();
            }
        } else if (id >= IDM_OUTPUT_BASE) {
            const size_t index = id - IDM_OUTPUT_BASE;
            if (index < outputIds_.size()) {
                Settings::setOutputDevice(outputIds_[index]);
                restartIfRunning();
            }
        } else if (id >= IDM_CAPTURE_BASE) {
            const size_t index = id - IDM_CAPTURE_BASE;
            if (index < captureIds_.size()) {
                Settings::setInputDevice(captureIds_[index]);
                restartIfRunning();
            }
        }
    }

    // Never exit while the engine is mid-teardown: the stop also flushes
    // ffmpeg and any dump file. The wait happens off the UI thread so the
    // tray stays responsive, and the icon disappears immediately so a slow
    // teardown does not look like a hang.
    void quit() {
        removeTrayIcon();
        tray_.hIcon = nullptr;
        if (engine_ && isActive(engine_->state())) {
            engine_->stop();
            HWND window = window_;
            Engine* engine = engine_.get();
            std::thread([engine, window] {
                engine->waitUntilStopped(8);
                PostMessageW(window, WMAPP_QUIT_READY, 0, 0);
            }).detach();
        } else {
            PostQuitMessage(0);
        }
    }

    // MARK: - Message handling

    static LRESULT CALLBACK windowProcThunk(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_CREATE) {
            const CREATESTRUCTW* create =
                reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return 0;
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        return self->windowProc(window, message, wParam, lParam);
    }

    LRESULT windowProc(HWND window, UINT message, WPARAM wParam,
                       LPARAM lParam) {
        switch (message) {
            case WMAPP_TRAY:
                switch (LOWORD(lParam)) {
                    case WM_LBUTTONUP:
                    case WM_RBUTTONUP:
                    case WM_CONTEXTMENU:
                        devicesFresh_ = false;
                        showMenu();
                        return 0;
                }
                return 0;
            case WM_COMMAND:
                handleCommand(LOWORD(wParam));
                return 0;
            case WMAPP_STATE: {
                const EngineState state = static_cast<EngineState>(wParam);
                updateTrayIcon(state);
                switch (state) {
                    case EngineState::running:
                        appendLog("Streaming.");
                        break;
                    case EngineState::idle:
                        appendLog("Stopped; device released.");
                        if (pendingRestart_) {
                            pendingRestart_ = false;
                            startEngine();
                        }
                        break;
                    case EngineState::failed: {
                        const std::string reason =
                            engine_ ? engine_->status().failureReason
                                    : std::string();
                        appendLog("Failed: " + reason);
                        pendingRestart_ = false;
                        presentFailure(reason);
                        break;
                    }
                    default: break;
                }
                return 0;
            }
            case WMAPP_LOG: {
                std::unique_ptr<std::string> line(
                    reinterpret_cast<std::string*>(lParam));
                if (line) appendLog(*line);
                return 0;
            }
            case WMAPP_QUIT_READY:
                PostQuitMessage(0);
                return 0;
            case WM_TIMER: {
                const EngineStatus status =
                    engine_ ? engine_->status() : EngineStatus();
                statusWindow_->update(
                    status,
                    engine_ ? engine_->liveInputLevels()
                            : std::vector<float>(),
                    Settings::sampleRate());
                if (status.state == EngineState::running) refreshTooltip();
                return 0;
            }
            case WM_SETTINGCHANGE:
                // Theme flips do not redraw a tray icon for free.
                updateTrayIcon(engine_ ? engine_->state() : EngineState::idle);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    NOTIFYICONDATAW tray_{};
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<StatusWindow> statusWindow_;
    bool pendingRestart_ = false;
    std::vector<std::string> captureIds_;
    std::vector<std::string> outputIds_;
    std::vector<DeviceInfo> devices_;
    bool devicesFresh_ = false;
};

}  // namespace ac3spdif

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    using namespace ac3spdif;

    if (wcsstr(commandLine, L"--service") != nullptr) {
        return ServiceMode::run();
    }

    // One tray icon per session is plenty.
    HANDLE instanceMutex =
        CreateMutexW(nullptr, TRUE, L"Local\\ac3spdif.app");
    if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    ComInit com;
    App app(instance);
    const int result = app.run();
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
    return result;
}
