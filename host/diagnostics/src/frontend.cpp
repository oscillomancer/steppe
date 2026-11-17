// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagnostics/frontend.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "native.h"
#include "storage.h"
#include "supple/base/paths.h"
#include "supple/diagctx/table.h"
#include "supple/product.h"
#include "terminal.h"
#include "trace.h"
namespace supple::diagnostics {
namespace detail {
bool valid(const Packet& p) {
    return p.magic == 0x43525348 && p.version == 1 && p.role >= 1 && p.role <= 3 &&
           p.kind >= Message::Boot && p.kind <= Message::Trace && p.incident[32] == 0 &&
           std::all_of(p.incident.begin(), p.incident.begin() + 32,
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}  // namespace detail
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
        const bool terminal = native.trusted && installTerminal(native, boot);
        Packet ready = boot;
        ready.kind = Message::Ready;
        ready.pid = pid();
        ready.cause = terminal ? 1U : 0U;
        setTableAddress(ready, &diagctx::table());
        ready.code = marker && *marker >= 0 ? 0U : 1U;
        if (ready.code) std::fputs("local diagnostic marker unavailable\n", stderr);
        (void)publish(native, ready);
        const int code = main(argc, argv);  // no catch/unwind on an unexpected exception
        Packet expected = boot;
        expected.kind = Message::Expected;
        expected.code = static_cast<std::uint32_t>(code);
        (void)publish(native, expected);
        // Keep marker and channel until actual process exit, including static
        // teardown: returning from main is not yet a clean process exit.
        (void)marker;
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
        if (packet.kind == Message::Terminal) {
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
                                       code, fatal, hasTerminal ? &trace : nullptr))
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
