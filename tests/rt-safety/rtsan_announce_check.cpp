// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Proves an inventory announcement (ADR-0083, topic 5c) cannot be made on an audio path
// unnoticed: with no sink installed, as in the engine tests, announcing from a SUPPLE_RT
// function must still be killed by RealtimeSanitizer. `audio` is registered as WILL_FAIL; `cold`
// makes the same calls outside a real-time context and must pass, so the failure above is the
// announcement and nothing else. rtsan preset only.
#include <cstdio>
#include <cstring>

#include <supple/base/rt.h>
#include <supple/diagctx/inventory.h>

namespace dc = supple::diagctx;

static void announceEverything() {
    const dc::Identity identity{"a", "b", "c", "d"};
    dc::announce(1, dc::InstanceKind::Clap, dc::InstanceOrigin::Foreign,
                 dc::InstanceMode::InProcess, dc::InstanceState::Loading, 0, identity);
    dc::announceState(1, dc::InstanceState::Active);
    dc::announceRemove(1);
}

static void announceOnAudioThread() SUPPLE_RT { announceEverything(); }

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "audio") == 0) {
        announceOnAudioThread();
        std::puts("rtsan did not fire");
        return 0;
    }
    announceEverything();
    std::puts("cold announcements ran");
    return 0;
}
