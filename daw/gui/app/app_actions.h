// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The app's actions: every command, the built-ins the default keymap names,
// the user's keymap, and key routing (gui.md §6). UI state the actions read
// (selection, playhead) comes from UiState.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <optional>
#include <string>
#include <vector>

#include "control_map.h"
#include "supple/actions/actions.h"
#include "supple/engine/app.h"

namespace supple::ui {
class Root;
}  // namespace supple::ui

namespace supple::gui {

// The launcher's clipboard: the clips copied or cut (Ctrl+C, Ctrl+X with the
// launcher's keys). Clip ids restart in each project, so it holds only for
// the project it was filled in, as the timeline clipboard does.
struct ClipClipboard {
    std::vector<std::string> clips;
    bool cut = false;              // Ctrl+X: the paste moves them
    std::uint64_t generation = 0;  // EngineApp::projectGeneration() when filled
    void fill(std::vector<std::string> ids, bool cutting, std::uint64_t projectGeneration);
    // The clips, or none once another project has opened (it is then emptied).
    const std::vector<std::string>& in(std::uint64_t projectGeneration);
};

// Per-project UI state (gui.md §1.2); persisted later to <project>.ui.json.
struct UiState {
    std::vector<std::string> selectedTracks, selectedItems;
    std::optional<std::string> trackAnchor;  // Shift+click extends from here
    bool tracksFocused = false;  // tracks were selected last (Delete removes them, not items)
    // An automation lane was pressed last: Delete and the clipboard keys take
    // its selected points.
    bool envelopeFocused = false;
    // The clip launcher's selected clips (gui.md §4.15); a launcher cell was
    // clicked last, so Delete removes them.
    std::vector<std::string> selectedClips;
    bool clipsFocused = false;
    ClipClipboard clipClipboard;
};

// Removes clips in one undo step (the launcher's Delete).
Status removeClips(cmd::Dispatcher& dispatcher, const std::vector<std::string>& clips);

// Pastes the launcher's clipboard from a cell (gui.md §4.15): each clip lands
// as far from (track, scene) as it was from the copied clips' top-left cell,
// in `channels` order (the view's) and the scenes' order, as copies
// (clip.duplicate) in one undo step. A clip that is gone, a cell past the
// last channel or scene, or an occupied one refuses the whole paste. With
// `move` (after a cut) the clips themselves move there (clip.move), front
// first, so new cells that overlap the old ones are free in time. The pasted
// clips' ids, in the clipboard's order.
Result<std::vector<std::string>> pasteClips(cmd::Dispatcher& dispatcher,
                                            const model::Session& session,
                                            const std::vector<std::string>& clips,
                                            const std::string& track, const std::string& scene,
                                            const std::vector<std::string>& channels,
                                            bool move = false);

// A click on a track in a view that lists `order` (its row or strip order):
// plain selects it alone, Ctrl toggles it, Shift extends from the anchor
// (gui.md §4.2). The TCP and the mixer share the selection.
void selectTrack(UiState& ui, const std::string& track, unsigned mods,
                 const std::vector<std::string>& order);
bool trackSelected(const UiState& ui, const std::string& track);

// The keymap's sections for a key on `root`, innermost first: "text" in a
// text field, the MIDI editor or the mixer when the pointer is over it (else
// when the focus is inside it: the pointer is elsewhere, over the bars or out
// of the window), then the arrange view (its defaults edit the selection,
// shared by every panel) and global bindings. So Delete over the arrange view
// removes items even while the MIDI editor has the focus, and over the editor
// removes notes.
std::vector<std::string> keySections(const ui::Root& root);

// GUI-only commands: `gui.selection` (read-only selection for agents, §1.2).
void registerGuiCommands(cmd::Registry& registry, const UiState& ui);

class AppActions {
  public:
    AppActions(const cmd::Registry& registry, cmd::Dispatcher& dispatcher, engine::EngineApp& app,
               UiState& ui, std::filesystem::path keymapFile,
               std::filesystem::path controlsFile = {});
    ~AppActions();

    actions::ActionRegistry& registry() { return registry_; }
    actions::Keymap& keymap() { return keymap_; }
    // Every binding back to the defaults; the keymap file then holds no changes.
    Status resetKeymap();
    UiState& ui() { return ui_; }
    engine::EngineApp& app() { return app_; }
    cmd::Dispatcher& dispatcher() { return dispatcher_; }
    const std::filesystem::path& keymapFile() const { return keymapFile_; }
    actions::Context context(std::vector<std::string> sections = {"arrange", "global"}) const;
    Result<json::Value> run(const actions::Action& action,
                            const json::Value& prompted = json::Value::object());
    Result<json::Value> run(std::string_view id);
    // From a menu, the toolbar or a key: runs the action, or, when it still
    // needs typed arguments the selection does not give, asks for them
    // (onPrompt opens the same form as the actions list).
    void invoke(const actions::Action& action);
    void invoke(std::string_view id);
    std::function<void(const actions::Action&)> onPrompt;
    // A key the focused widget did not use: true when it ran (or started) a
    // binding. `typing` (a text field has focus) lets only modified chords
    // through; `sections` are the keymap's, innermost first.
    bool key(int keycode, unsigned mods, bool typing,
             const std::vector<std::string>& sections = {"arrange", "global"});
    // Built-in UI actions the app provides (open the actions list, …).
    void addUi(std::string id, std::string name, std::function<void()> run);
    // Menus grey the action while `when` is false (Action::enabled).
    void setEnabled(std::string_view id, std::function<bool(const actions::Context&)> when);
    // Ticks the action in menus while `when` holds (a View toggle).
    void setChecked(std::string_view id, std::function<bool(const actions::Context&)> when);
    // A built-in with one prompted argument (the actions list asks for it).
    void addUi(std::string id, std::string name, std::string prompt,
               std::function<Status(const json::Value& value)> run);
    // A built-in that acts on the selection and is never prompted: a caller
    // (a strip's own button) may aim it elsewhere with a value under `key`,
    // which `run` receives (null when none was given).
    void addUiFor(std::string id, std::string name, std::string key,
                  std::function<Status(const json::Value& value)> run);
    // Shown by the app (status line): the last action's error, if any.
    std::function<void(const std::string&)> onError;
    // Shown by the app (status line): what an edit did, when it is news (a
    // bake's "Riff baked: 6 notes written as they sound").
    std::function<void(const std::string&)> onNotice;
    // The action's first key binding ("Ctrl+Z", or ""), for menus; with
    // all, every binding ("Ctrl+Shift+Z, Ctrl+Y"), for the actions list.
    std::string keysFor(std::string_view action, bool all = false) const;

    // MIDI learn for actions (ADR-0027): the controls bound to them, saved
    // in controlsFile() (nothing is saved without one).
    ControlMap& controls() { return controls_; }
    const std::filesystem::path& controlsFile() const { return controlsFile_; }
    Status saveControls() const;
    std::string controlsFor(std::string_view action) const;  // "CC 21 · ch 1", or ""
    // The devices' messages: a press runs its action once (a note on, a CC
    // above 0, an encoder step); a "(MIDI CC)" action follows the value.
    void control(const std::vector<engine::EngineApp::ControlEvent>& events);
    // One app-wide learn dialog across roots. Replacement closes the old
    // dialog; its deferred destruction cannot end the new owner's session.
    using LearnSink = std::function<void(const std::vector<engine::EngineApp::ControlEvent>&)>;
    std::uint64_t beginLearn(LearnSink sink, std::function<void()> close);
    void endLearn(std::uint64_t owner);
    LearnSink learnSink;  // active listener (also read by tests)

    // The "(MIDI CC)" action that follows a knob for `action`, when `action`
    // only runs per press (a command whose value a knob cannot pass); "" if none.
    std::string followingControl(std::string_view action) const;
    // The playrate knob's range (kRateRanges in playrate_range.h), chosen in
    // the transport bar's menu; "Transport: playrate (MIDI CC)" follows it.
    std::size_t playrateRange = 0;
    // The send whose level or pan the routing matrix moved last (ADR-0058),
    // for "Routing: last touched send (MIDI CC)"; none once it is removed.
    struct TouchedSend {
        model::Id route;
        std::string param;  // volume or pan
    };
    void touchSend(model::Id route, std::string param);
    std::optional<TouchedSend> lastTouchedSend() const;
    // A built-in that follows a controller (listed as "… (MIDI CC)").
    void addControl(std::string id, std::string name, std::string description,
                    std::function<Status(const actions::ControlValue&)> run,
                    actions::SoftTakeover takeover = actions::SoftTakeover::None);

  private:
    std::uint64_t learnSerial_ = 0, learnOwner_ = 0;
    std::function<void()> closeLearn_;
    actions::ActionRegistry registry_;
    actions::Keymap keymap_;
    std::filesystem::path keymapFile_;
    ControlMap controls_;
    std::filesystem::path controlsFile_;
    struct ControlState {
        std::uint64_t lastMs = 0, serial = 0;
    };
    std::map<std::string, ControlState, std::less<>> controlState_;
    // Soft takeover's memory, per value a followed action moves ("fx_2:gain",
    // "control.track.volume:tr_1").
    std::map<std::string, engine::Takeover> takeover_;
    // An absolute `v`'s knob position (0..1) for the value `key` now at
    // `current`; with soft takeover, none until the knob reaches it. `step`:
    // one step of a stepped value.
    std::optional<double> knob(const actions::ControlValue& v, const std::string& key,
                               double current, double step = 0);
    // What `v` left the value at (0..1): a knob holds it from there.
    void held(const actions::ControlValue& v, const std::string& key, double value);
    std::optional<TouchedSend> touchedSend_;
    // Track volume / pan of the selection (or the master) by a controller.
    Result<json::Value> controlTracks(const actions::ControlValue& v, bool pan, bool master);
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    actions::Runner runner_;
};

}  // namespace supple::gui
