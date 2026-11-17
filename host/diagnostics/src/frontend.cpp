// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagnostics/frontend.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "inventory.h"
#include "native.h"
#include "storage.h"
#include "supple/base/paths.h"
#include "supple/diagctx/table.h"
#include "supple/product.h"
#include "terminal.h"
#include "trace.h"
#include "watch.h"
namespace supple::diagnostics {
namespace detail {
bool valid(const Packet& p) {
    return p.magic == 0x43525348 && p.version == kProtocolVersion && p.role >= 1 && p.role <= 3 &&
           p.kind >= Message::Boot && p.kind <= Message::Instance && p.incident[32] == 0 &&
           std::all_of(p.incident.begin(), p.incident.begin() + 32,
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}  // namespace detail
namespace {
// What the worker's exit handler needs to remove its run marker once the process is truly
// going: a launcher's stop leaves no supervisor to do it (ADR-0091). Built before the handler is
// registered, so that it outlives the handler (which runs first of the two).
struct OwnMarker {
    std::filesystem::path directory;
    detail::Packet boot;
    std::intptr_t file = -1;
    std::atomic<bool> mainReturned{false};  // the orderly end; an exit() from elsewhere is not
};
OwnMarker& ownMarker() {
    static OwnMarker marker;
    return marker;
}
void removeOwnMarkerAtExit() {
    auto& marker = ownMarker();
    if (marker.mainReturned.load() && marker.file >= 0 &&
        detail::supervisorEnd() == detail::SupervisorEnd::Launcher)
        detail::removeOwnMarker(marker.directory, marker.boot, marker.file);
}
}  // namespace

int runFrontend(int argc, char** argv, Role role, Main main, Options options) {
    using namespace detail;
    if (options.directory.empty()) {
        const auto data = paths::localData();
        if (!data.empty()) options.directory = data / std::string(product::kSlug) / "diagnostics";
    }
    const bool storage = !options.directory.empty() && prepareDirectory(options.directory);
    if (argc >= 3 && std::string_view(argv[argc - 2]) == "--diagnostic-worker") {
        Packet boot;
        auto adopted = adopt(argv[argc - 1], boot);
        if (!adopted || boot.role != static_cast<unsigned>(role)) {
            std::fputs("local diagnostic bootstrap unavailable\n", stderr);
            argc -= 2;
            argv[argc] = nullptr;
            return main(argc, argv);
        }
        auto native = *adopted;
        argc -= 2;
        argv[argc] = nullptr;
        const auto marker = storage && native.trusted ? createMarker(options.directory, boot)
                                                      : Result<std::intptr_t>(std::intptr_t{-1});
        // The marker's removal on a launcher's stop runs from an exit handler registered here,
        // as early as there is a marker: handlers and static destructors run in the reverse of
        // the order they were set up, so it runs after the destructors of everything built
        // later (the engine, the program's own statics) and removes the marker as late as this
        // code can. A crash in that teardown then still leaves it, as the end it is.
        if (marker && *marker >= 0) {
            auto& own = ownMarker();
            own.directory = options.directory;
            own.boot = boot;
            own.file = *marker;
            (void)std::atexit(&removeOwnMarkerAtExit);
        }
        const bool terminal = native.trusted && installTerminal(native, boot);
        Packet ready = boot;
        ready.kind = Message::Ready;
        ready.pid = pid();
        ready.cause = terminal ? 1U : 0U;
        setTableAddress(ready, &diagctx::table());
        ready.code = marker && *marker >= 0 ? 0U : 1U;
        if (ready.code) std::fputs("local diagnostic marker unavailable\n", stderr);
        (void)publish(native, ready);
        // The inventory goes over the same channel, so only where the channel is trusted.
        if (native.trusted) installInventory(native, boot);
        // From here a supervisor that disappears without the "collector failed" notice is a
        // launcher's stop, and the program stops as if it had been sent SIGTERM (ADR-0091).
        startSupervisorWatch(native);
        const int code = main(argc, argv);  // no catch/unwind on an unexpected exception
        Packet expected = boot;
        expected.kind = Message::Expected;
        expected.code = static_cast<std::uint32_t>(code);
        (void)publish(native, expected);
        // The orderly end. On a launcher's stop (ADR-0091) the supervisor is gone and cannot
        // remove the marker, so the exit handler registered above does, once the process is
        // really ending (after static teardown); on the hard-exit path no handler runs and the
        // marker stays. Where a supervisor is alive it removes the marker itself.
        ownMarker().mainReturned.store(true);
        return code;
    }
    auto id = incidentId();
    if (!id) {
        std::fputs("local diagnostics unavailable\n", stderr);
        return main(argc, argv);
    }
    Packet boot;
    boot.role = static_cast<unsigned>(role);
    boot.pid = pid();
    std::copy(id->begin(), id->end(), boot.incident.begin());
    auto child = launch(argc, argv, boot);
    if (!child) {
        std::fputs("local diagnostics unavailable\n", stderr);
        return main(argc, argv);
    }
    auto native = *child;
    closeProtocolStdio();
    bool hasTerminal = false, markerUnavailable = false, ownsMarker = false;
    Inventory inventory;
    std::optional<std::uint32_t> expected;
    Packet packet, terminal;
    TraceSnapshot trace;
    while (receive(native, packet)) {
        if (packet.incident != boot.incident || packet.role != boot.role) break;
        if (packet.kind == Message::Ready) {
            native.table = tableAddress(packet);
            markerUnavailable = packet.code != 0;
            ownsMarker = !markerUnavailable;
        }
        if (packet.kind == Message::Expected) expected = packet.code;
        if (const auto hook = supervisorHook()) hook(packet);  // tests only
        if (packet.kind == Message::Instance) {
            (void)inventory.apply(packet);  // damage is counted inside, never fatal
            continue;
        }
        if (packet.kind == Message::Terminal) {
            inventory.noteDropped(packet.dropped);
            terminal = packet;
            hasTerminal = true;
            // Read the other threads inside the worker's terminal hold: the collector
            // has its own deadline, below the hold, and releases every thread it
            // stopped before the acknowledgement. Formatting and file work come after.
            trace = collectProcessTrace(native, packet);
            acknowledge(native);
        }
    }
    bool fatal = false;
    const int code = wait(native, fatal, hasTerminal ? std::nullopt : expected);
    close(native);
    if (markerUnavailable) std::fputs("diagnostic coverage was partial\n", stderr);
    if (hasTerminal || fatal || !expected) {
        if (!storage || !publishReport(options.directory, boot, hasTerminal ? &terminal : nullptr,
                                       code, fatal, hasTerminal ? &trace : nullptr, &inventory))
            std::fputs(
                "the app closed unexpectedly; local report publication or retention failed\n",
                stderr);
        else if (ownsMarker)
            removeMarker(options.directory, boot);
    } else if (storage && ownsMarker)
        removeMarker(options.directory, boot);
    return code;
}
}  // namespace supple::diagnostics
