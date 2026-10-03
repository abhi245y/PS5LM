// llama.cpp's tools leave through _Exit once main returns (issue #3).
//
// After main, the SDK's start code unloads the Sony modules the payload loaded
// and calls Sony's exit() (ps5-payload-dev/sdk, crt/crt.c), which then runs
// every C++ static destructor registered with __cxa_atexit while other threads
// may still run. "[SceLibc] A heap error is detected" shows up at exit, after
// the work is done, which points at this phase. Native PS5 apps never return
// from main (ProsperoAI,
// ps5-native-app-boilerplate); a payload can end with _Exit, as SDK payloads
// did until 2024 (sdk commit f19dd3e).
//
// ps5/CMakeLists.txt links this into the tools with --wrap=main: the start
// code's call to main lands here, and __real_main is the tool's own main.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "log.h"

#include <cstdio>
#include <cstdlib>

extern "C" int __real_main(int argc, char ** argv, char ** envp);
extern "C" int __wrap_main(int argc, char ** argv, char ** envp);

int __wrap_main(int argc, char ** argv, char ** envp) {
    const int rc = __real_main(argc, argv, envp);
    // llama.cpp logs through a worker thread: drain it, then stdio.
    common_log_flush(common_log_main());
    fflush(nullptr);
    _Exit(rc);
}
