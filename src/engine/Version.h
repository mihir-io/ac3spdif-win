// The version lives in one place. The CLI and the app read these constants
// directly, and build.ps1 greps the literal out of this file for the .exe
// version resources, so nothing can drift from the binaries.
//
// The build stamp is kept alongside deliberately. A version number identifies
// a release but does not change between rebuilds of the same one, so on its
// own it cannot distinguish a stale binary from a fix that did not work —
// which is a mistake that has already been made once in this project's
// history.

#pragma once

namespace ac3spdif {

inline constexpr const char* kVersion = "0.1.0";
inline const char* buildStamp() { return __DATE__ " " __TIME__; }

}  // namespace ac3spdif
