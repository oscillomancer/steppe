// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Actions, keymaps and macros (docs/specs/gui.md §6). An action is a command
// with bound arguments (strings starting with "$" resolve from the context:
// the selection, the playhead), a macro (actions run as one batch, one undo
// step) or a built-in UI function. The command registry generates the base
// set, so every command is searchable and bindable.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/json/value.h"

namespace supple::actions {

// What "$…" arguments resolve against when an action runs.
struct Context {
    std::vector<std::string> selectedTracks, selectedItems;  // ids, in order
    double playheadSeconds = 0, playheadBeats = 0;
    std::vector<std::string> sections{"global"};   // innermost first: {"arrange", "global"}
    json::Value prompted = json::Value::object();  // a built-in's prompted arguments
};

// A controller's message for an action that follows it (MIDI learn,
// ADR-0027): its position 0..1, or signed encoder steps.
struct ControlValue {
    double value = 0;
    int steps = 0;
    bool relative = false;
    // A knob with soft takeover (Action::takeover): one far from the value
    // waits until it reaches it.
    bool softTakeover = false;
    std::string gesture;  // one undo step per movement: the origin's gesture
};

// Whether a followed action's knob takes over softly: never (a press, an
// encoder), when its binding asks (MIDI learn offers it), or always.
enum class SoftTakeover : std::uint8_t { None, Offered, Always };

struct Action {
    std::string id;    // "history.undo", "track.add:buss", "macro:…", "ui.actions"
    std::string name;  // "Track: add buss"
    std::string description;
    std::string command;  // a registry command; empty for macros and built-ins
    // Bound arguments. Placeholders: "$selection.track(s)", "$selection.item(s)",
    // "$playhead" (seconds), "$playhead.beats"; "$each.track" / "$each.item"
    // run the command once per selected id, all in one transaction.
    json::Value args = json::Value::object();
    std::vector<std::string> prompts;  // arguments asked for when run
    std::vector<std::string> steps;    // a macro's action ids
    std::function<Result<json::Value>(const Context&)> builtin;
    // Set: the action follows a controller's value ("(MIDI CC)" actions:
    // a volume, a pan, a scroll) instead of running once per press.
    std::function<Result<json::Value>(const ControlValue&, const Context&)> control;
    SoftTakeover takeover = SoftTakeover::None;
    // Set: whether the action applies now (menus grey it when false). Unset: always.
    std::function<bool(const Context&)> enabled;
    // Set: whether the action's choice holds now (menus tick it). Unset: never.
    std::function<bool(const Context&)> checked;
    bool fromRegistry = false;
};

class ActionRegistry {
  public:
    // One action per exposed command: required arguments named track(s)/item(s)
    // bind to the selection; others become prompts.
    void addCommands(const cmd::Registry& registry);
    void add(Action action);  // replaces an action with the same id
    const Action* find(std::string_view id) const;
    std::vector<const Action*> all() const;

    struct Match {
        const Action* action;
        int score;
    };
    // Fuzzy search over names, ids and descriptions (subsequence, ranked).
    std::vector<Match> search(std::string_view query, std::size_t limit = 50) const;

  private:
    std::map<std::string, Action, std::less<>> actions_;
};

// Fuzzy subsequence score of `query` in `text` (case-insensitive); none if
// the query's characters do not all appear in order.
std::optional<int> fuzzyScore(std::string_view query, std::string_view text);

// Runs actions through the dispatcher with Origin::Ui.
class Runner {
  public:
    Runner(const ActionRegistry& registry, cmd::Dispatcher& dispatcher)
        : registry_(registry), dispatcher_(dispatcher) {}
    Result<json::Value> run(const Action& action, const Context& context,
                            const json::Value& prompted = json::Value::object());
    Result<json::Value> run(std::string_view id, const Context& context);
    // Resolves "$…" placeholders in `args`.
    static Result<json::Value> resolve(const json::Value& args, const Context& context);

  private:
    Result<json::Value> step(const Action& action, const Context& context,
                             const json::Value& prompted);
    const ActionRegistry& registry_;
    cmd::Dispatcher& dispatcher_;
    int depth_ = 0;  // macro nesting while a step runs
};

// --- keys

struct Chord {
    int key = 0;        // SDL keycode
    unsigned mods = 0;  // ui::Mod bits: 1 shift, 2 ctrl, 4 alt, 8 super
    bool operator==(const Chord&) const = default;
    auto operator<=>(const Chord&) const = default;
};
std::string describe(const Chord& chord);                // "Ctrl+Shift+Z"
std::optional<Chord> parseChord(std::string_view text);  // inverse of describe

struct Binding {
    std::vector<Chord> keys;  // one chord, or a two-chord sequence
    std::string section;      // "global", "arrange", "midi", "mixer", "fx"
    std::string action;
    bool operator==(const Binding&) const = default;
};

class Keymap {
  public:
    static Keymap defaults();
    // Defaults with the user's changes (supple.keymap/1.0: "bind", "unbind").
    static Result<Keymap> load(const std::filesystem::path& file);
    Status save(const std::filesystem::path& file) const;  // only the changes
    static std::filesystem::path defaultFile(std::string_view slug);

    void bind(Binding binding);  // replaces any binding of the same keys in that section
    void unbind(const Binding& binding);
    const std::vector<Binding>& bindings() const { return bindings_; }
    std::vector<Binding> bindingsFor(std::string_view action) const;
    // Pairs of bindings with the same keys in the same section.
    std::vector<std::pair<Binding, Binding>> conflicts() const;

    // Feeds one chord; returns the action when a binding completes in the
    // innermost section that has one. A chord that starts a sequence is
    // held (pending() true) until the next one.
    std::optional<std::string> press(const Chord& chord, const std::vector<std::string>& sections);
    bool pending() const { return !held_.empty(); }
    void clearPending() { held_.clear(); }

  private:
    std::vector<Binding> bindings_, defaults_;
    std::vector<Chord> held_;
};

}  // namespace supple::actions
