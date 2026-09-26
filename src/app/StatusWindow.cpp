#include "StatusWindow.h"

#include "../engine/Version.h"
#include "../support/Win32Util.h"

#include <cmath>
#include <cstdio>

namespace ac3spdif {

static const wchar_t* kClassName = L"ac3spdif.status";
static const int kLogHeight = 180;
static const int kMargin = 12;
static const int kRowHeight = 19;

// The client height is the sum of everything paint() lays out, not a number
// that looked right once: thirteen status rows, the latency block, a meter
// block reserved for eight channels — the widest mix Windows presents is 7.1
// — the version stamp, and the log. A mix wider than the reservation
// compresses its meter rows to fit rather than running into the text below,
// which is the failure this arithmetic replaced.
static const int kStatusRows = 13;
static const int kLatencyRows = 7;  // header + six terms
static const int kMeterRowHeight = 13;
static const int kReservedMeterRows = 8;
static const int kMeterBlockHeight =
    kRowHeight + kReservedMeterRows * kMeterRowHeight;
static const int kStampHeight = 16;
static const int kTopAreaHeight =
    kMargin + (kStatusRows + kLatencyRows) * kRowHeight + 3 * 6 +
    kMeterBlockHeight + 6 + kStampHeight;
static const int kClientHeight =
    kTopAreaHeight + kMargin + kLogHeight + kMargin;

StatusWindow::StatusWindow(HINSTANCE instance) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);

    RECT frame = {0, 0, 520, kClientHeight};
    AdjustWindowRect(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    window_ = CreateWindowExW(
        0, kClassName, L"ac3spdif — status",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
        instance, this);

    RECT client;
    GetClientRect(window_, &client);
    logEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        kMargin, client.bottom - kLogHeight - kMargin,
        client.right - 2 * kMargin, kLogHeight, window_, nullptr, instance,
        nullptr);

    font_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        VARIABLE_PITCH, L"Segoe UI");
    monoFont_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            FIXED_PITCH, L"Consolas");
    SendMessageW(logEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont_), TRUE);
}

StatusWindow::~StatusWindow() {
    if (window_) DestroyWindow(window_);
    if (font_) DeleteObject(font_);
    if (monoFont_) DeleteObject(monoFont_);
}

void StatusWindow::show() {
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
}

void StatusWindow::update(const EngineStatus& status,
                          const std::vector<float>& liveLevels,
                          double sampleRate) {
    status_ = status;
    liveLevels_ = liveLevels;
    sampleRate_ = sampleRate;
    if (IsWindowVisible(window_)) {
        RECT client;
        GetClientRect(window_, &client);
        client.bottom -= kLogHeight + 2 * kMargin;
        InvalidateRect(window_, &client, TRUE);
    }
}

void StatusWindow::append(const std::string& line) {
    const std::wstring wide = widen(line) + L"\r\n";
    const int length = GetWindowTextLengthW(logEdit_);
    // Keep the log from growing without bound over a long run.
    if (length > 60000) SetWindowTextW(logEdit_, L"");
    SendMessageW(logEdit_, EM_SETSEL, length, length);
    SendMessageW(logEdit_, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(wide.c_str()));
}

LRESULT CALLBACK StatusWindow::windowProc(HWND window, UINT message,
                                          WPARAM wParam, LPARAM lParam) {
    StatusWindow* self = reinterpret_cast<StatusWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_CREATE: {
            const CREATESTRUCTW* create =
                reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paintStruct;
            HDC dc = BeginPaint(window, &paintStruct);
            if (self) {
                RECT client;
                GetClientRect(window, &client);
                self->paint(dc, client);
            }
            EndPaint(window, &paintStruct);
            return 0;
        }
        case WM_CLOSE:
            // The window is a view on a running engine, not a document; hide
            // rather than destroy, so the log keeps accumulating.
            ShowWindow(window, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static std::wstring format(const char* pattern, ...) {
    char buffer[256];
    va_list arguments;
    va_start(arguments, pattern);
    vsnprintf(buffer, sizeof(buffer), pattern, arguments);
    va_end(arguments);
    return widen(buffer);
}

void StatusWindow::paint(HDC dc, const RECT& client) {
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);
    const COLORREF label = GetSysColor(COLOR_GRAYTEXT);
    const COLORREF value = GetSysColor(COLOR_WINDOWTEXT);

    int y = kMargin;
    const int labelX = kMargin;
    const int valueX = kMargin + 130;
    const int lineHeight = kRowHeight;
    const auto row = [&](const wchar_t* name, const std::wstring& text) {
        SetTextColor(dc, label);
        TextOutW(dc, labelX, y, name, static_cast<int>(wcslen(name)));
        SetTextColor(dc, value);
        TextOutW(dc, valueX, y, text.c_str(), static_cast<int>(text.size()));
        y += lineHeight;
    };

    const EngineStatus& s = status_;
    row(L"State", widen(stateLabel(s.state)) +
                      (s.state == EngineState::failed && !s.failureReason.empty()
                           ? L" — " + widen(s.failureReason)
                           : L""));
    row(L"Capture", widen(s.inputName));
    row(L"Output", widen(s.outputName));
    row(L"Format", widen(s.format));
    row(L"Carrier", widen(s.carrier));
    row(L"On the wire", format("%.1f s · %lld bursts", s.secondsOnWire,
                               static_cast<long long>(s.bursts)));
    row(L"PCM margin", s.targetMarginMs > 0
                           ? format("%.1f ms at the trough (target %d ms)",
                                    s.marginMs, s.targetMarginMs)
                           : std::wstring(L"— (self-test has no capture)"));
    row(L"Drift", format("%+.0f ppm", s.driftPPM));
    row(L"Dropouts", format("%lld", static_cast<long long>(s.dropouts)));
    row(L"Encode time", format("%.2f ms average, %.2f ms worst",
                               s.encodeMsAverage, s.encodeMsMax));
    row(L"Capture overflow", format("%lld frames",
                                    static_cast<long long>(s.captureOverflowFrames)));
    row(L"Silence frames", format("%lld  (nothing playing, or dropouts)",
                                  static_cast<long long>(s.silenceFrames)));
    row(L"Channels seen", s.sourceChannels > 0
                              ? format("%d in the mix, %d captured",
                                       s.sourceChannels, s.capturedChannels)
                              : std::wstring(L"—"));

    y += 6;
    SetTextColor(dc, label);
    TextOutW(dc, labelX, y, L"Latency (measured)", 18);
    y += lineHeight;
    const LatencyBreakdown& l = s.latency;
    const auto latencyRow = [&](const wchar_t* name, int frames) {
        row(name, format("%d frames · %.1f ms", frames,
                         l.milliseconds(frames, sampleRate_)));
    };
    latencyRow(L"  capture buffer", l.captureBufferFrames);
    latencyRow(L"  pcm margin", l.pcmMarginFrames);
    latencyRow(L"  encoder frame", l.encoderFrames);
    latencyRow(L"  encoder delay", l.encoderDelayFrames);
    latencyRow(L"  output buffer", l.outputBufferFrames);
    latencyRow(L"  total", l.totalFrames());

    y += 6;
    const int stampY = client.bottom - kLogHeight - 2 * kMargin - kStampHeight;
    drawMeters(dc, labelX, y, client.right - 2 * kMargin, stampY - 6);

    // The build stamp distinguishes a stale binary from a fix that did not
    // work; the version alone cannot.
    SetTextColor(dc, label);
    const std::wstring stamp =
        format("ac3spdif %s · built %s", kVersion, buildStamp());
    TextOutW(dc, labelX, stampY, stamp.c_str(),
             static_cast<int>(stamp.size()));
}

// The name of the position a channel carries, from the mix format's speaker
// mask: WASAPI channel order follows the mask's bit order, so the index-th
// set bit *is* the index-th channel. Labels match the surround positions the
// macOS meter view uses; a mask that does not describe the stream — fewer
// bits than channels, or none at all — falls back to plain numbering rather
// than guessing.
static std::wstring channelLabel(size_t index, uint32_t mask,
                                 size_t channelCount) {
    static const wchar_t* names[] = {
        L"FL",  L"FR",  L"FC",  L"LFE", L"BL",  L"BR",
        L"FLC", L"FRC", L"BC",  L"SL",  L"SR",  L"TC",
        L"TFL", L"TFC", L"TFR", L"TBL", L"TBC", L"TBR"};
    size_t seen = 0;
    size_t popcount = 0;
    for (uint32_t bits = mask; bits != 0; bits &= bits - 1) ++popcount;
    if (popcount >= channelCount) {
        for (size_t bit = 0; bit < 18; ++bit) {
            if ((mask & (1u << bit)) == 0) continue;
            if (seen == index) return names[bit];
            ++seen;
        }
    }
    wchar_t number[8];
    _snwprintf_s(number, _TRUNCATE, L"%02zu", index + 1);
    return number;
}

void StatusWindow::drawMeters(HDC dc, int x, int& y, int width, int maxY) {
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    TextOutW(dc, x, y, L"Channel activity", 16);
    y += kRowHeight;
    if (liveLevels_.empty()) {
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        TextOutW(dc, x, y, L"—", 1);
        y += kRowHeight;
        return;
    }

    SelectObject(dc, monoFont_);
    const int barLeft = x + 34;
    const int barWidth = width - 40;
    // The window reserves room for eight rows; a mix wider than that gets
    // shorter rows rather than rows that spill over the text below.
    int rowHeight = kMeterRowHeight;
    const int available = maxY - y;
    if (static_cast<int>(liveLevels_.size()) * rowHeight > available &&
        !liveLevels_.empty()) {
        rowHeight = available / static_cast<int>(liveLevels_.size());
        if (rowHeight < 7) rowHeight = 7;
    }
    HBRUSH track = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    HBRUSH fill = CreateSolidBrush(RGB(80, 160, 80));
    HBRUSH hot = CreateSolidBrush(RGB(200, 60, 60));
    HBRUSH discard = CreateSolidBrush(RGB(150, 150, 150));

    for (size_t channel = 0; channel < liveLevels_.size(); ++channel) {
        if (y + rowHeight > maxY) break;
        const bool discarded =
            channel >= static_cast<size_t>(status_.capturedChannels);
        std::wstring label = channelLabel(channel, status_.sourceChannelMask,
                                          liveLevels_.size());
        if (discarded) label += L"!";
        TextOutW(dc, x, y, label.c_str(), static_cast<int>(label.size()));

        RECT trackRect = {barLeft, y + 2, barLeft + barWidth, y + rowHeight - 2};
        FillRect(dc, &trackRect, track);

        // dBFS over -50..0; a linear meter never leaves the first pixels for
        // anything but full-scale content.
        const float level = liveLevels_[channel];
        float fraction = 0;
        if (level > 0.003f) {
            const float db = 20.0f * std::log10(level);
            fraction = (db + 50.0f) / 50.0f;
            if (fraction < 0.02f) fraction = 0.02f;
            if (fraction > 1) fraction = 1;
        }
        if (fraction > 0) {
            RECT levelRect = trackRect;
            levelRect.right =
                barLeft + static_cast<int>(barWidth * fraction);
            FillRect(dc, &levelRect,
                     discarded ? discard : (level > 0.891f ? hot : fill));
        }
        y += rowHeight;
    }
    DeleteObject(track);
    DeleteObject(fill);
    DeleteObject(hot);
    DeleteObject(discard);
    SelectObject(dc, font_);
    y += 6;
}

}  // namespace ac3spdif
