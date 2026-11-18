// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal (route E stage 5): a compiled plan as canonical text, to tell
// whether two plans hold the same compiled description. Not a public
// interface.
#pragma once

#include <string>
#include <string_view>

namespace supple::engine::detail {
struct Plan;

// Main thread, before the plan is published: every field of the prepared
// plan that compile and prepare decide, one per line ("n2 tr_3 chain0 fx_4
// latency 64"), in plan order. Runtime state (cursors, ramps, voices, held
// notes, queues, buffer contents, meters, the executors' handoff fields) is
// left out, and so are addresses: an instance is named by its FX or module.
// The same session compiled twice with the same pool dumps the same text.
std::string dumpPlan(const Plan& p);

// The first line where two dumps differ ("line 12: <a> | <b>"); empty when
// they are equal.
std::string firstDifference(std::string_view a, std::string_view b);
}  // namespace supple::engine::detail
