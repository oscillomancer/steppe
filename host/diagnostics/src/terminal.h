// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "native.h"
namespace supple::diagnostics::detail {
bool installTerminal(Native native, const Packet& boot);
#if defined(_WIN32)
// Whether this module's last-chance filter is the process's top-level filter right now.
// installTerminal reports success only if it is; the fixture prints it as evidence.
bool lastChanceFilterInstalled();
#endif
}  // namespace supple::diagnostics::detail
