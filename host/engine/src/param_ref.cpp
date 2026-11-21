// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "param_ref.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <vector>

namespace supple::engine::detail {
namespace {

bool foldedEqual(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

bool digits(std::string_view s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string_view::npos;
}

const json::Value* paramsOf(const json::Value* schema) {
    if (!schema || !schema->isObject()) return nullptr;
    const json::Value* ps = schema->find("params");
    return ps && ps->isObject() ? ps : nullptr;
}

std::optional<std::uint32_t> idOf(const json::Value& entry) {
    if (!entry.isObject()) return std::nullopt;
    const json::Value* v = entry.find("id");
    if (!v || !v->isNumber()) return std::nullopt;
    return static_cast<std::uint32_t>(v->asInt());
}

// A parameter named by id or name: stored as the semantic key describing it
// when one does (one spelling per parameter, so identity is equality of
// stored references), otherwise as its decimal id. `semantic` stays null:
// the values given with this spelling are plain.
ParamRef canonicalOf(const host::ParamInfo& info, const json::Value* params) {
    ParamRef r;
    r.info = &info;
    r.id = info.id;
    r.canonical = std::to_string(info.id);
    if (params)
        for (const auto& entry : params->asObject())
            if (idOf(entry.value) == info.id) {
                r.canonical = entry.key;
                r.described = &entry.value;
                break;
            }
    return r;
}

std::string quoted(std::string_view s) { return "\"" + std::string(s) + "\""; }

}  // namespace

Result<ParamRef> resolveParam(std::span<const host::ParamInfo> list, const json::Value* schema,
                              std::string_view ref, std::string_view plugin, Ambiguity ambiguity) {
    const json::Value* params = paramsOf(schema);
    // A semantic key, exactly (one may be all digits; it wins over an id).
    if (params)
        if (const json::Value* entry = params->find(ref); entry && entry->isObject()) {
            ParamRef r;
            r.canonical = std::string(ref);
            r.semantic = r.described = entry;
            r.id = idOf(*entry);
            if (r.id)
                for (const host::ParamInfo& info : list)
                    if (info.id == *r.id) r.info = &info;
            return r;
        }
    if (digits(ref)) {
        if (ref.size() > 1 && ref.front() == '0') {
            const auto first = ref.find_first_not_of('0');
            const std::string canonical =
                first == std::string_view::npos ? "0" : std::string(ref.substr(first));
            return Error("param.id", "parameter ids have no leading zeros: " + quoted(ref), "",
                         "use the canonical parameter id " + canonical);
        }
        std::uint32_t number = 0;
        const auto [end, ec] = std::from_chars(ref.data(), ref.data() + ref.size(), number);
        if (ec == std::errc{} && end == ref.data() + ref.size())
            for (const host::ParamInfo& info : list)
                if (info.id == number) return canonicalOf(info, params);
        return Error("param.unknown", std::string(plugin) + " has no parameter " + quoted(ref), "",
                     "fx.describe lists its parameter keys, ids and names");
    }
    std::vector<const host::ParamInfo*> matches, exact;
    for (const host::ParamInfo& info : list)
        if (foldedEqual(info.name, ref)) {
            matches.push_back(&info);
            if (info.name == ref) exact.push_back(&info);
        }
    if (matches.empty())
        return Error("param.unknown", std::string(plugin) + " has no parameter " + quoted(ref), "",
                     "fx.describe lists its parameter keys, ids and names");
    if (matches.size() == 1) return canonicalOf(*matches.front(), params);
    if (exact.size() == 1) return canonicalOf(*exact.front(), params);
    if (ambiguity == Ambiguity::First)
        return canonicalOf(exact.empty() ? *matches.front() : *exact.front(), params);
    constexpr std::size_t kNamed = 8;
    std::string named;
    for (std::size_t i = 0; i < matches.size() && i < kNamed; ++i)
        named += (i ? ", " : "") + std::to_string(matches[i]->id) + " (" + matches[i]->name + ")";
    if (matches.size() > kNamed)
        named += " and " + std::to_string(matches.size() - kNamed) + " more";
    return Error("param.ambiguous",
                 quoted(ref) + " names " + std::to_string(matches.size()) + " parameters of " +
                     std::string(plugin),
                 "", "name one by its id: " + named);
}

Error recodeParamError(Error e, std::string code) {
    if (e.code != "param.unknown") e.code = std::move(code);
    return e;
}

}  // namespace supple::engine::detail
