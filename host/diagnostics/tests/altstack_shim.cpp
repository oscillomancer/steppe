// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
//
// Fault injection for the alternate signal stack's restore (review round 31). A test
// preloads this library into the diagnostics fixture; while SUPPLE_ALTSTACK_FAIL_RESTORE
// is set, every sigaltstack call that disables the alternate stack fails with EPERM, as a
// restore that cannot complete at thread exit would. The fixture's teardown mode then shows
// whether the thread's own stack was unmapped while still enabled (the kernel answers the
// next signal frame with a forced SIGILL and no handler) or kept mapped.
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>

extern "C" int sigaltstack(const stack_t* ss, stack_t* old) {
    using Fn = int (*)(const stack_t*, stack_t*);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "sigaltstack"));
    if (ss && (ss->ss_flags & SS_DISABLE) != 0 && std::getenv("SUPPLE_ALTSTACK_FAIL_RESTORE")) {
        static const char note[] = "shim: restore refused\n";
        (void)!::write(2, note, sizeof note - 1);
        errno = EPERM;
        return -1;
    }
    return real(ss, old);
}
