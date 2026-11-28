// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/actions/actions.h"

#include <algorithm>
#include <cctype>

namespace supple::actions {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

// "track.add" → "Track: add"; the summary becomes the description.
std::string titleOf(const std::string& command) {
    std::string out;
    const auto dot = command.find('.');
    std::string group = dot == std::string::npos ? command : command.substr(0, dot);
    std::string rest = dot == std::string::npos ? std::string() : command.substr(dot + 1);
    if (!group.empty())
        group[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(group[0])));
    std::replace(rest.begin(), rest.end(), '.', ' ');
    std::replace(rest.begin(), rest.end(), '-', ' ');
    std::replace(rest.begin(), rest.end(), '_', ' ');
    return rest.empty() ? group : group + ": " + rest;
}

bool isPlaceholder(const json::Value& v) {
    return v.isString() && !v.asString().empty() && v.asString()[0] == '$';
}

}  // namespace

// --- registry

void ActionRegistry::addCommands(const cmd::Registry& registry) {
    for (const cmd::CommandInfo* info : registry.all()) {
        if (info->exposure == cmd::Exposure::Internal) continue;
        Action a;
        a.id = info->name;
        a.name = titleOf(info->name);
        a.description = info->summary;
        a.command = info->name;
        a.fromRegistry = true;
        if (const json::Value* required = info->argsSchema.find("required");
            required && required->isArray())
            for (const json::Value& r : required->asArray()) {
                const std::string& arg = r.asString();
                if (arg == "track")
                    a.args[arg] = "$selection.track";
                else if (arg == "tracks")
                    a.args[arg] = "$selection.tracks";
                else if (arg == "item")
                    a.args[arg] = "$selection.item";
                else if (arg == "items")
                    a.args[arg] = "$selection.items";
                else
                    a.prompts.push_back(arg);
            }
        add(std::move(a));
    }
}

void ActionRegistry::add(Action action) {
    std::string id = action.id;
    actions_.insert_or_assign(std::move(id), std::move(action));
}

const Action* ActionRegistry::find(std::string_view id) const {
    auto it = actions_.find(id);
    return it == actions_.end() ? nullptr : &it->second;
}

std::vector<const Action*> ActionRegistry::all() const {
    std::vector<const Action*> out;
    for (const auto& [id, a] : actions_) out.push_back(&a);
    return out;
}

std::optional<int> fuzzyScore(std::string_view query, std::string_view text) {
    if (query.empty()) return 0;
    int score = 0, run = 0;
    std::size_t at = 0;
    bool first = true;
    for (char qc : query) {
        if (qc == ' ') continue;
        const char q = static_cast<char>(std::tolower(static_cast<unsigned char>(qc)));
        bool found = false;
        for (; at < text.size(); ++at) {
            const char t = static_cast<char>(std::tolower(static_cast<unsigned char>(text[at])));
            if (t != q) {
                run = 0;
                continue;
            }
            const bool wordStart =
                at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
            score += 1 + (wordStart ? 8 : 0) + run * 4 + (first && at == 0 ? 6 : 0);
            ++run;
            ++at;
            found = true;
            break;
        }
        if (!found) return std::nullopt;
        first = false;
    }
    return score - static_cast<int>(text.size() / 16);  // shorter texts win ties
}

std::vector<ActionRegistry::Match> ActionRegistry::search(std::string_view query,
                                                          std::size_t limit) const {
    std::vector<Match> out;
    for (const auto& [id, a] : actions_) {
        std::optional<int> best;
        for (auto [text, weight] :
             {std::pair<std::string_view, int>{a.name, 3}, {a.id, 2}, {a.description, 1}})
            if (auto s = fuzzyScore(query, text))
                best = std::max(best.value_or(-1000000), *s * weight);
        if (best) out.push_back({&a, *best});
    }
    std::stable_sort(out.begin(), out.end(), [](const Match& x, const Match& y) {
        return x.score != y.score ? x.score > y.score : x.action->name < y.action->name;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

// --- runner

Result<json::Value> Runner::resolve(const json::Value& args, const Context& c) {
    if (!args.isObject()) return args;
    json::Value out = json::Value::object();
    for (const auto& m : args.asObject()) {
        if (!isPlaceholder(m.value)) {
            out[m.key] = m.value;
            continue;
        }
        const std::string& p = m.value.asString();
        auto ids = [](const std::vector<std::string>& v) {
            json::Value a = json::Value::array();
            for (const auto& id : v) a.push_back(id);
            return a;
        };
        if (p == "$selection.track" || p == "$selection.item") {
            const auto& v = p == "$selection.track" ? c.selectedTracks : c.selectedItems;
            if (v.empty())
                return Error("action.selection",
                             std::string("select a ") +
                                 (p == "$selection.track" ? "track" : "item") + " first",
                             "/" + m.key);
            out[m.key] = v.front();
        } else if (p == "$selection.tracks") {
            out[m.key] = ids(c.selectedTracks);
        } else if (p == "$selection.items") {
            out[m.key] = ids(c.selectedItems);
        } else if (p == "$playhead") {
            out[m.key] = c.playheadSeconds;
        } else if (p == "$playhead.beats") {
            out[m.key] = c.playheadBeats;
        } else if (p == "$each.track" || p == "$each.item") {
            out[m.key] = p;  // expanded by the runner
        } else {
            return Error("action.placeholder", "unknown placeholder " + p, "/" + m.key);
        }
    }
    return out;
}

Result<json::Value> Runner::step(const Action& a, const Context& c, const json::Value& prompted) {
    if (a.builtin) {
        for (const std::string& p : a.prompts)
            if (!prompted.isObject() || !prompted.find(p))
                return Error("action.prompt", a.name + " needs \"" + p + "\"", "/" + p);
        Context withArgs = c;
        withArgs.prompted = prompted;
        return a.builtin(withArgs);
    }
    if (!a.steps.empty()) {
        // The macro's values reach every step; each takes the ones its own
        // prompts name. Macros may nest, but not without end.
        if (depth_ >= 16)
            return Error("action.macro_depth", a.name + " nests macros more than 16 deep", {},
                         "remove the macro step that leads back to itself");
        ++depth_;
        json::Value results = json::Value::array();
        for (const std::string& id : a.steps) {
            const Action* s = registry_.find(id);
            if (!s) {
                --depth_;
                return Error("action.unknown", "macro step " + id + " is not an action");
            }
            auto r = step(*s, c, prompted);
            if (!r) {
                --depth_;
                return r;
            }
            results.push_back(std::move(*r));
        }
        --depth_;
        return results;
    }
    auto args = resolve(a.args, c);
    if (!args) return args;
    for (const std::string& p : a.prompts) {
        const json::Value* v = prompted.isObject() ? prompted.find(p) : nullptr;
        if (!v) return Error("action.prompt", a.name + " needs \"" + p + "\"", "/" + p);
        (*args)[p] = *v;
    }
    // "$each.*": once per selected id.
    for (const auto& m : args->asObject()) {
        if (!m.value.isString() || m.value.asString().rfind("$each.", 0) != 0) continue;
        const bool tracks = m.value.asString() == "$each.track";
        const auto& ids = tracks ? c.selectedTracks : c.selectedItems;
        if (ids.empty())
            return Error("action.selection",
                         std::string("select ") + (tracks ? "tracks" : "items") + " first");
        const std::string key = m.key;
        json::Value results = json::Value::array();
        for (const std::string& id : ids) {
            json::Value one = *args;
            one[key] = id;
            auto r = dispatcher_.execute({a.command, std::move(one), kOrigin});
            if (!r) {
                refused_ = a.command;
                return r;
            }
            results.push_back(std::move(*r));
        }
        return results;
    }
    auto r = dispatcher_.execute({a.command, std::move(*args), kOrigin});
    if (!r) refused_ = a.command;
    return r;
}

Result<json::Value> Runner::run(const Action& a, const Context& c, const json::Value& prompted) {
    refused_.clear();
    bool many = !a.steps.empty();
    if (a.args.isObject())
        for (const auto& m : a.args.asObject())
            many = many || (m.value.isString() && m.value.asString().rfind("$each.", 0) == 0);
    if (!many || a.builtin) return step(a, c, prompted);
    return dispatcher_.transact(a.name, kOrigin,
                                [&] { return step(a, c, prompted); });  // one undo step
}

Result<json::Value> Runner::run(std::string_view id, const Context& c) {
    const Action* a = registry_.find(id);
    if (!a) return Error("action.unknown", "no action " + std::string(id));
    return run(*a, c);
}

}  // namespace supple::actions
