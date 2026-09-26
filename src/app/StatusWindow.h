// The status window: live buffer, drift, underrun and burst figures, a
// per-channel meter block, and the log.
//
// On macOS the channel meters hid behind the Option key in the menu; a Win32
// popup menu has no equivalent bargain to make (and no live-updating custom
// views without owner-draw contortions), so the meters live here instead,
// always visible while the window is open. They are diagnostics, and the
// window is where diagnostics go.
//
// The meters read Engine::liveInputLevels via the refresh timer rather than
// the status snapshot — the snapshot refreshes five times a second, which is
// ample for buffer and drift figures but makes a meter visibly step rather
// than move.

#pragma once

#include "../engine/Engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace ac3spdif {

class StatusWindow {
public:
    // Created hidden so the log can accumulate from the first engine line;
    // show() is what the menu item calls.
    explicit StatusWindow(HINSTANCE instance);
    ~StatusWindow();

    void show();
    void update(const EngineStatus& status, const std::vector<float>& liveLevels,
                double sampleRate);
    void append(const std::string& line);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    void paint(HDC dc, const RECT& client);
    // Meters draw into the space between `y` and `maxY`, compressing their
    // rows when the mix is wider than the reserved block.
    void drawMeters(HDC dc, int x, int& y, int width, int maxY);

    HWND window_ = nullptr;
    HWND logEdit_ = nullptr;
    HFONT font_ = nullptr;
    HFONT monoFont_ = nullptr;
    EngineStatus status_;
    std::vector<float> liveLevels_;
    double sampleRate_ = 48000;
};

}  // namespace ac3spdif
