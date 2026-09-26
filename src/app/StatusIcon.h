// The tray icon: a TOSLINK socket, drawn at runtime.
//
// The socket is the outer form — square with the top two corners chamfered,
// the keying that stops a TOSLINK plug going in upside down — and inside it
// sits a single circle: the metal ferrule on the end of the cable. Nothing
// else, because at sixteen pixels anything else is noise.
//
// The ferrule carries the state, the way the real connector does:
//
//   stopped     hollow — nothing in the socket
//   busy        solid, in the taskbar's own colour (starting/stopping)
//   streaming   red — the LED you can see glowing in a live connector
//   failed      hollow, struck through
//
// Windows has no template-image machinery to fight with the way macOS does;
// the icon simply picks black or white from the taskbar theme, redrawn when
// the app is told the theme changed (WM_SETTINGCHANGE), and the streaming
// state keeps its red regardless — a glance should tell you it is live.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ac3spdif {

enum class IconState { stopped, busy, streaming, failed };

namespace StatusIcon {

// A freshly drawn HICON at the system's small-icon size. The caller owns it
// and destroys the previous one after Shell_NotifyIcon has the new one.
HICON create(IconState state);

}  // namespace StatusIcon
}  // namespace ac3spdif
