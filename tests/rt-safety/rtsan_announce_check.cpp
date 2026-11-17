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
    dc::announceBridge(1);
    dc::announceBridge(1, dc::BridgeHow::Signal, 11);
}

static void announceOnAudioThread() SUPPLE_RT { announceEverything(); }
// A bridge's start and end (topic 6a) alone: each must be refused on its own, not only after
// the instance announcements above have already aborted the process.
static void announceBridgeOnAudioThread() SUPPLE_RT { dc::announceBridge(1); }
static void announceBridgeEndOnAudioThread() SUPPLE_RT {
    dc::announceBridge(1, dc::BridgeHow::Signal, 11);
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "audio") == 0) {
        announceOnAudioThread();
        std::puts("rtsan did not fire");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "audio-bridge") == 0) {
        announceBridgeOnAudioThread();
        std::puts("rtsan did not fire");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "audio-bridge-end") == 0) {
        announceBridgeEndOnAudioThread();
        std::puts("rtsan did not fire");
        return 0;
    }
    announceEverything();
    std::puts("cold announcements ran");
    return 0;
}
