// Background service mode — the Windows spelling of the macOS LaunchAgent.
//
// "Run at login" on Windows is a Run-key entry, and the entry points at this
// same executable with --service: the engine runs headless, no tray icon and
// no window, restarting itself after a failure the way launchd's KeepAlive
// would. One binary, two front ends, one pipeline.
//
// The app and the service are mutually exclusive by nature: whichever claims
// the output device exclusively first wins, and the other cannot start. A
// named mutex makes the conflict legible before WASAPI turns it into an
// AUDCLNT_E_DEVICE_IN_USE — the app checks for a running service and offers
// to stop it (via a named event the service waits on) rather than leaving the
// user to decode an exclusive-mode error.

#pragma once

#include <string>

namespace ac3spdif {
namespace ServiceMode {

// The Run-key entry: installed = starts at login.
bool isInstalled();
void install();    // throws Failure
void uninstall();

// Whether a --service instance is running right now, in this session.
bool isRunning();
// Ask a running service to stop, and wait briefly for it to let go of the
// device. Returns true once it has.
bool requestStop();

// The --service entry point. Returns the process exit code.
int run();

}  // namespace ServiceMode
}  // namespace ac3spdif
