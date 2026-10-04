/*
 * PSFlyCast - the title's process start.
 *
 * Based on ps5-native-app-boilerplate's tooling/native/app_crt.cpp
 * (Copyright (C) 2026 BlackBearReloaded), with PS5 RetroArch's BSS clear
 * (Copyright (C) 2026 Mihawk) and PS5_Vulkan's IEEE floating-point state
 * (Copyright (C) 2026 Mihawk-99).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's loader does not zero the BSS: the tail of the writable segment
 * past the file image holds whatever the memory held before (PS5 RetroArch
 * found its signal counter at -285230512 before any signal). Flycast's BSS is
 * about 25 MiB of zero-initialised objects - mutexes, flags, pointers, the
 * emulator's state - so it is cleared here, before anything can read it.
 */

#include <cstddef>
#include <cstdint>

using Destructor = void (*)();
using Initializer = void (*)();

extern "C"
{
    void _init_env(void *process_parameters);
    void ps5_fp_ieee(void);
    int atexit(Destructor callback);
    [[noreturn]] void exit(int status);
    int application_main(int argc, char **argv, char **envp) __asm__("main");
    void catchReturnFromMain(int status);

    extern Initializer __preinit_array_start[] __attribute__((weak));
    extern Initializer __preinit_array_end[] __attribute__((weak));
    extern Initializer __init_array_start[] __attribute__((weak));
    extern Initializer __init_array_end[] __attribute__((weak));
    extern Initializer __fini_array_start[] __attribute__((weak));
    extern Initializer __fini_array_end[] __attribute__((weak));

    /* shell/ps5/runtime/ps5-title.ld */
    extern char __bss_start[];
    extern char __bss_end[];
}

namespace
{
void run_forward(Initializer *first, Initializer *last) noexcept
{
    if (first == nullptr || last == nullptr)
        return;
    while (first != last)
        (*first++)();
}

void run_reverse(Initializer *first, Initializer *last) noexcept
{
    if (first == nullptr || last == nullptr)
        return;
    while (last != first)
        (*--last)();
}

/* A plain loop the compiler may not turn into a memset call: nothing has run
 * yet, so only code in this file is relied on. */
void zero_bss() noexcept
{
    for (volatile char *at = __bss_start; at != __bss_end; at++)
        *at = 0;
}
} // namespace

extern "C" void _init()
{
    run_forward(__preinit_array_start, __preinit_array_end);
    run_forward(__init_array_start, __init_array_end);
}

extern "C" void _fini()
{
    run_reverse(__fini_array_start, __fini_array_end);
}

extern "C" [[noreturn]] __attribute__((visibility("default"))) void
_start(void *process_parameters, Destructor loader_teardown)
{
    zero_bss();

    const int argc = *static_cast<const int *>(process_parameters);
    auto *parameters = static_cast<std::uint8_t *>(process_parameters);
    auto **argv = reinterpret_cast<char **>(parameters + sizeof(std::uint64_t));

    /* The console starts a title with flush-to-zero and denormals-are-zero
     * on; the code linked here expects the IEEE state (PS5 platform, fp.h). */
    ps5_fp_ieee();
    _init_env(process_parameters);
    if (loader_teardown != nullptr)
        (void)atexit(loader_teardown);
    (void)atexit(_fini);
    _init();
    const int status = application_main(argc, argv, nullptr);
    /* A title must not exit(): shell/ps5/ps5_main.cpp asks the shell to close
     * it and does not come back. */
    catchReturnFromMain(status);
    exit(status);
}
