// Argument parsing and help text. Lives with the CLI, not the engine — the
// tray app builds its Config from the registry and has no use for any of this.

#pragma once

#include "../engine/Config.h"

namespace ac3spdif {

struct ParsedArguments {
    Config config;
    bool list = false;
    bool help = false;
    // 0 = run until Ctrl-C.
    double durationSeconds = 0;
};

namespace CLI {

extern const char* usage;

// Throws Failure on a malformed argument. Prints and exits for --version,
// which is a query, not a run.
ParsedArguments parse(int argc, wchar_t** argv);

}  // namespace CLI

}  // namespace ac3spdif
