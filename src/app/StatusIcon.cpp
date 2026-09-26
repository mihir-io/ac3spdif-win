#include "StatusIcon.h"

#include <objidl.h>

// GDI+ for one reason: it composes straight into a 32-bit alpha surface, which
// is what an antialiased tray icon needs and what plain GDI cannot produce
// without hand-managing the alpha channel.
#pragma warning(push)
#pragma warning(disable : 4458)
#include <gdiplus.h>
#pragma warning(pop)

namespace ac3spdif {
namespace StatusIcon {

static bool taskbarIsLight() {
    // The taskbar follows the *system* theme, not the app theme, and dark is
    // the norm — so the icon defaults to white unless the registry says the
    // system chrome is light.
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                      L"Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD light = 0, size = sizeof(light);
    RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, nullptr,
                     reinterpret_cast<BYTE*>(&light), &size);
    RegCloseKey(key);
    return light != 0;
}

static void ensureGdiplus() {
    static ULONG_PTR token = 0;
    if (token) return;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    // Deliberately never shut down: the icon is drawn for the life of the
    // process and GDI+ teardown during DLL unload is its own famous bug.
}

HICON create(IconState state) {
    ensureGdiplus();

    const int size = GetSystemMetrics(SM_CXSMICON) >= 20 ? 32 : 16;
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const Gdiplus::Color base =
        taskbarIsLight() ? Gdiplus::Color(255, 0, 0, 0)
                         : Gdiplus::Color(255, 255, 255, 255);
    const Gdiplus::Color live(255, 235, 45, 45);

    const float s = static_cast<float>(size);
    const float stroke = s > 16 ? 2.5f : 1.5f;

    // The connector face: wider than tall, with *small* 45° chamfers on the
    // top corners — the keying that stops a TOSLINK plug going in upside
    // down. The chamfer size is the whole likeness: at 30% of the width the
    // top edge all but vanished and the outline read as a pentagon; at 18%
    // the top edge survives and it reads as a plug face again.
    const float left = s * 0.07f, right = s - s * 0.07f;
    const float top = s * 0.16f, bottom = s - s * 0.12f;
    const float chamfer = s * 0.18f;

    Gdiplus::GraphicsPath socket;
    Gdiplus::PointF points[] = {
        {left + chamfer, top}, {right - chamfer, top}, {right, top + chamfer},
        {right, bottom},       {left, bottom},         {left, top + chamfer},
    };
    socket.AddPolygon(points, 6);
    Gdiplus::Pen socketPen(base, stroke);
    socketPen.SetLineJoin(Gdiplus::LineJoinMiter);
    graphics.DrawPath(&socketPen, &socket);

    // The ferrule, centred in the face.
    const float radius = s * 0.16f;
    const float cx = s / 2, cy = (top + bottom) / 2 + s * 0.02f;
    const Gdiplus::RectF ferrule(cx - radius, cy - radius, radius * 2,
                                 radius * 2);
    switch (state) {
        case IconState::stopped: {
            Gdiplus::Pen pen(base, stroke);
            graphics.DrawEllipse(&pen, ferrule);
            break;
        }
        case IconState::busy: {
            Gdiplus::SolidBrush brush(base);
            graphics.FillEllipse(&brush, ferrule);
            break;
        }
        case IconState::streaming: {
            Gdiplus::SolidBrush brush(live);
            graphics.FillEllipse(&brush, ferrule);
            break;
        }
        case IconState::failed: {
            Gdiplus::Pen pen(base, stroke);
            graphics.DrawEllipse(&pen, ferrule);
            graphics.DrawLine(&pen, left + s * 0.06f, bottom - s * 0.02f,
                              right - s * 0.06f, top + s * 0.10f);
            break;
        }
    }

    HICON icon = nullptr;
    bitmap.GetHICON(&icon);
    return icon;
}

}  // namespace StatusIcon
}  // namespace ac3spdif
