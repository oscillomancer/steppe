// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A plugin made from a DSP script (supple/script_code.h: compiled by the
// player, or by a C++ compiler in a source export) as a framework Definition
// with a meaning (supple/suite/semantics.h).
//
//   - Identity from the script's Info: its ID, name, vendor, version, kind
//     and buses. The state tag is made from the ID, so it is as permanent as
//     the ID is.
//   - A parameter per Param member, with the script's CLAP ID, ranges, unit,
//     curve, group and flags. A continuous parameter's CLAP value is its
//     0..1 position (normalizedHostValue()), as Steppe's semantic hosts keep
//     it; the plain value is what the script reads (a percent as a
//     fraction), so the framework hands it over as it is.
//   - The meaning: each parameter's key (the member's name in snake case,
//     or withKey()), its roles as "role:<name>" tags, choices by key.
//   - Modulation: the script's Source members and, for an instrument, the
//     runtime's own sources (velocity, modwheel ...); its modulated()
//     parameters move by routes from global sources, an instrument's
//     polyModulated() ones by any. With sources and parameters to move, the
//     routes are a property of their own (kRoutesProperty), the patch's
//     "routes" member, which factory patches set too.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "supple/script_code.h"
#include "supple/suite/semantics.h"

namespace supple::suite {

class ScriptDefinition final : public SemanticDefinition {
  public:
    ScriptDefinition(PluginInfo info, ParamTable params, PropertyList properties, Meaning meaning,
                     std::shared_ptr<const script::detail::Code> code, std::filesystem::path file,
                     std::string source, std::vector<FactoryPreset> presets = {})
        : SemanticDefinition(std::move(info), std::move(params), std::move(properties),
                             std::move(meaning), std::move(presets)),
          code_(std::move(code)),
          file_(std::move(file)),
          source_(std::move(source)) {}

    const std::shared_ptr<const script::detail::Code>& code() const { return code_; }
    // Where it came from (empty when there is nothing to follow: an export),
    // and its text (empty when it was compiled as C++).
    const std::filesystem::path& file() const { return file_; }
    const std::string& source() const { return source_; }

  private:
    std::shared_ptr<const script::detail::Code> code_;
    std::filesystem::path file_;
    std::string source_;
};

// The Definition of a script's plugin; null when it would break the
// framework's rules (Definition::problems() says why, in `problems`).
std::shared_ptr<const ScriptDefinition> makeScriptDefinition(
    std::shared_ptr<const script::detail::Code> code, std::filesystem::path file = {},
    std::string source = {}, std::string* problems = nullptr);

// The routes property's ID (Meaning::routes): one no file member's key
// gives, since "routes" is the patch's own member.
inline constexpr uint32_t kRoutesProperty = script::detail::keyId("routes");

// Whether two plugins are the same to a host and to an agent: identity,
// parameters, files, factory patches, meaning, buses, declared latency and
// tail. DSP-only edits swap in place without a restart.
bool sameShape(const Definition& a, const Definition& b);

// Four ASCII characters made from an ID, for the state tag.
std::string stateTagFor(std::string_view id);

}  // namespace supple::suite
