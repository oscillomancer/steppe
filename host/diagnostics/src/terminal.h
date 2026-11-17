// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "native.h"
namespace supple::diagnostics::detail {
bool installTerminal(Native native, const Packet& boot);
#if defined(_WIN32)
// Test support: whether this module's last-chance filter is the process's top-level
// filter, that is, whether installTerminal did not decline to replace an earlier one.
bool lastChanceFilterInstalled();
#endif
}  // namespace supple::diagnostics::detail
