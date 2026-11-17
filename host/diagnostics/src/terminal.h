// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "native.h"
namespace supple::diagnostics::detail {
bool installTerminal(Native native, const Packet& boot);
// Whether a terminal handler has begun on any thread. The supervisor watch (watch.cpp)
// leaves a crashing process alone.
bool terminalBegun() noexcept;
#if defined(_WIN32)
// Whether this module's last-chance filter is the process's top-level filter right now.
// Test evidence only (the fixture prints it): installTerminal does not call it, because
// reading the filter means clearing it for a moment.
bool lastChanceFilterInstalled();
#endif
}  // namespace supple::diagnostics::detail
