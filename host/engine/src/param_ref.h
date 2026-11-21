// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: one resolver for a reference to a plug-in parameter
// (session-model "Plug-in parameter references"), shared by envelopes,
// modulation routes, fx.param.set, cables into plugin modules and the plan.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "supple/base/result.h"
#include "supple/host/clap_host.h"
#include "supple/json/value.h"

namespace supple::engine::detail {

struct ParamRef {
    // The parameter in the list; null only for a semantic key whose id the
    // list lacks (the caller decides).
    const host::ParamInfo* info = nullptr;
    // What the model stores, one spelling per parameter: the semantic key
    // describing it (on a semantic plug-in, whatever the caller wrote),
    // otherwise the decimal id.
    std::string canonical;
    // The parameter's id: what identity compares (two spellings of one
    // parameter are one parameter). Absent only for a semantic key without one.
    std::optional<std::uint32_t> id;
    // The schema entry when the caller spelled the semantic key: the values
    // given with this reference are physical. Null for an id or a name,
    // whose values are plain (session-model "Plug-in parameter references").
    const json::Value* semantic = nullptr;
    // The schema entry of the key `canonical` names, however it was spelled
    // (null: no key describes the parameter).
    const json::Value* described = nullptr;
    // Spelled other than as stored: an id or name a key describes (the
    // caller's plain values convert to the key's physical ones).
    bool respelled() const { return described && !semantic; }
    std::string name() const { return info && !info->name.empty() ? info->name : canonical; }
};

// Commands refuse an ambiguous name; a reference already stored (an older
// file's display name) binds the first match, as it always did.
enum class Ambiguity { Refuse, First };

// Resolves `ref` against the instance's parameter `list` and its semantic
// `schema` (null or not an object: none). Accepts a semantic key exactly, a
// decimal id without leading zeros, or a display name ignoring case (a
// single exact-case match wins among several). Errors: `param.unknown`,
// `param.ambiguous` (the hint names the matches with their ids) and
// `param.id` (leading zeros; the hint gives the canonical id). `plugin` is
// the plug-in's display name, for the messages.
Result<ParamRef> resolveParam(std::span<const host::ParamInfo> list, const json::Value* schema,
                              std::string_view ref, std::string_view plugin,
                              Ambiguity ambiguity = Ambiguity::Refuse);

// `e` with its code replaced unless it is `param.unknown`, which callers
// explain with their own hints.
Error recodeParamError(Error e, std::string code);

}  // namespace supple::engine::detail
