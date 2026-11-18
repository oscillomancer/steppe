// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "safe_load.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include "supple/diagnostics/reports.h"
#include "supple/product.h"
#include "supple/ui/canvas.h"
#include "supple/ui/forms.h"
#include "supple/ui/popup.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {
const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::string textAt(const json::Value& v, const char* key) {
    const json::Value* x = v.isObject() ? v.find(key) : nullptr;
    return x && x->isString() ? x->asString() : std::string();
}
bool flagAt(const json::Value& v, const char* key) {
    const json::Value* x = v.isObject() ? v.find(key) : nullptr;
    return x && x->isBool() && x->asBool();
}

// Wrapped text that measures at the width it is given: a parent measures its children at its own
// full width, so a block that must wrap at the form's inner width says so itself.
class FixedWidthText : public ui::TextBlock {
  public:
    FixedWidthText(std::string id, std::string text, ui::Font font, std::string size,
                   std::string color, float width)
        : TextBlock(std::move(id), std::move(text), font, std::move(size), std::move(color)) {
        layout.width = ui::SizeRule::fixed(width);
    }
    ui::Size measure(const ui::Canvas& canvas, ui::Size available) override {
        return TextBlock::measure(canvas, {layout.width.value, available.h});
    }
};

// One of the choices, in a radio list: a 12-unit ring at the left, filled for the chosen one, a
// bold label and under it one quiet line. A click or Space chooses it, Up and Down move the choice,
// Enter goes on with it.
class RadioRow : public ui::Node {
  public:
    RadioRow(std::string id, std::string label, std::string what, std::function<void()> choose,
             std::function<void(int)> step, std::function<void()> confirm)
        : Node(std::move(id), ui::Role::Button),
          label_(std::move(label)),
          what_(std::move(what)),
          choose_(std::move(choose)),
          step_(std::move(step)),
          confirm_(std::move(confirm)) {
        interactive = true;
        focusable = true;
        layout.height = ui::SizeRule::fixed(40);
    }
    bool selected = false;
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        if (focused()) c.strokeRound({b.x, b.y, b.w, b.h}, t.radius("s"), t.color("focus"));
        const ui::Point centre{b.x + 14, b.y + 12};
        c.strokeCircle(centre, 6, t.color(selected ? "ink" : "ink-faint"), 1.2f);
        if (selected) c.fillCircle(centre, 3.4f, t.color("ink"));
        const float small = t.type("small"), caption = t.type("caption");
        c.text({b.x + 32, b.y + 4, b.w - 36, small * 1.4f}, label_, ui::Font::UiBold, small,
               t.color("ink"));
        c.text({b.x + 32, b.y + 4 + small * 1.4f, b.w - 36, caption * 1.4f}, what_, ui::Font::Ui,
               caption, t.color("ink-quiet"));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.button != 1) return false;
        if (e.type == ui::PointerEvent::Type::Down) {
            pressed_ = true;
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Up) {
            const bool fire = pressed_ && bounds().contains(e.pos);
            pressed_ = false;
            if (fire && choose_) choose_();
            return true;
        }
        return false;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
            if (confirm_) confirm_();
            return true;
        }
        if (e.key == SDLK_SPACE) {
            if (choose_) choose_();
            return true;
        }
        if (e.key == SDLK_DOWN || e.key == SDLK_UP) {
            if (step_) step_(e.key == SDLK_DOWN ? 1 : -1);
            return true;
        }
        return false;
    }
    std::string valueText() const override {
        return std::string(selected ? "(x) " : "( ) ") + label_ + " | " + what_;
    }

  private:
    std::string label_, what_;
    bool pressed_ = false;
    std::function<void()> choose_;
    std::function<void(int)> step_;
    std::function<void()> confirm_;
};

}  // namespace

void openSafeLoadDialog(ui::Root& root, const LoadQuestion& q,
                        std::function<void(LoadChoice, bool remember)> done,
                        std::function<void()> cancelled) {
    root.closePopups();
    struct State {
        bool decided = false;
        ui::Checkbox* remember = nullptr;
        std::vector<RadioRow*> rows;
        std::vector<LoadChoice> choices;
        std::size_t chosen = 0;
    };
    auto state = std::make_shared<State>();
    constexpr float kWidth = 520, kInner = kWidth - 32;
    auto form = std::make_unique<ui::Node>("safeload");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(kWidth);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(16);
    form->layout.gap = 8;
    const std::string app(product::kName);
    // The title: a long plug-in name may take a second line.
    form->emplace<FixedWidthText>("safeload.title",
                                  q.name + " was running when " + app + " closed unexpectedly",
                                  ui::Font::UiBold, "body", "ink", kInner);
    // The question in ink; what to keep in mind sits under the answers, quiet.
    std::string sentence, caveat;
    if (q.stock) {
        sentence = "It comes with " + app + ", so this looks like a fault in " + app +
                   " rather than in the plug-in. How should it load this time?";
        caveat = "A report cannot prove what caused it.";
    } else {
        sentence = "This project uses it. How should it load this time?";
        caveat = q.build == "unreadable"
                     ? "A report cannot prove the plug-in was the cause, and " + app +
                           " could not check that this is the build the report names."
                 : q.build == "version"
                     ? "A report cannot prove the plug-in was the cause. It names no build, so "
                       "this matches the version only."
                     : "A report cannot prove the plug-in was the cause.";
        if (!q.bridgeable) caveat += " A separate process is not available for it here.";
    }
    form->emplace<FixedWidthText>("safeload.sentence", sentence, ui::Font::Ui, "small", "ink",
                                  kInner);
    auto finish = [state, &root, done] {
        if (state->decided) return;
        state->decided = true;
        const bool remember =
            state->remember && state->remember->visible && state->remember->checked();
        const LoadChoice choice = state->choices[state->chosen];
        root.closePopups();
        if (done) done(choice, remember);
    };
    auto choose = [state](std::size_t index) {
        state->chosen = index;
        for (std::size_t i = 0; i < state->rows.size(); ++i) {
            state->rows[i]->selected = i == index;
            state->rows[i]->invalidate();
        }
    };
    const auto step = [state, &root, choose](int by) {
        const auto next = static_cast<long>(state->chosen) + by;
        if (next < 0 || next >= static_cast<long>(state->rows.size())) return;
        choose(static_cast<std::size_t>(next));
        root.focus(state->rows[static_cast<std::size_t>(next)]);
    };
    auto add = [&](const char* id, LoadChoice choice, const char* label, const std::string& what) {
        const std::size_t index = state->rows.size();
        state->choices.push_back(choice);
        state->rows.push_back(&form->emplace<RadioRow>(
            id, label, what, [choose, index] { choose(index); }, step, finish));
    };
    // The recommended one is chosen at open: a separate process where there can be one, else
    // disabling (a stock plug-in is never bridged).
    if (q.bridgeable && !q.stock)
        add("safeload.bridge", LoadChoice::Bridge, "Run it in a separate process",
            "If it stops again, " + app + " keeps running");
    add("safeload.disable", LoadChoice::Disable, "Disable it this time",
        "Audio passes through. The project keeps the plug-in");
    add("safeload.normal", LoadChoice::Normal, "Load it normally",
        "As before. " + app + " may close again if it is at fault");
    choose(0);
    form->emplace<FixedWidthText>("safeload.caveat", caveat, ui::Font::Ui, "caption", "ink-quiet",
                                  kInner);
    auto& buttons = form->emplace<ui::Node>("safeload.buttons");
    buttons.layout.dir = ui::Layout::Dir::Row;
    buttons.layout.height = ui::SizeRule::fixed(28);
    buttons.layout.gap = 8;
    buttons.layout.align = ui::Layout::Align::Center;
    if (q.rememberable()) {
        const std::string what =
            q.version.empty() ? "this build of " + q.name : q.name + " " + q.version;
        state->remember = &buttons.emplace<ui::Checkbox>("safeload.remember",
                                                         "Remember my choice for " + what, false);
        state->remember->outline = "ink-faint";
    }
    auto& space = buttons.emplace<ui::Node>("safeload.space");
    space.layout.width = ui::SizeRule::fraction();
    // Cancel closes the popup, and the popup's close handler is what cancels.
    auto& cancel =
        buttons.emplace<ui::Button>("safeload.cancel", "Cancel", [&root] { root.closePopups(); });
    cancel.quiet = true;
    cancel.layout.width = ui::SizeRule::fixed(84);
    auto& go = buttons.emplace<ui::Button>("safeload.continue", "Continue", finish);
    go.layout.width = ui::SizeRule::fixed(96);
    RadioRow* first = state->rows.front();
    root.openPopup(std::move(form), {}, ui::Placement::Center, [state, cancelled] {
        if (!state->decided && cancelled) {
            state->decided = true;
            cancelled();
        }
    });
    root.focus(first);
}

// ------------------------------------------------------------------ the check

struct SafeLoad::Run {
    json::Value found = json::Value::object();
    std::vector<LoadQuestion> questions;
    struct Answer {
        LoadQuestion question;
        LoadChoice choice;
        bool remember;
    };
    std::vector<Answer> answers;
    std::vector<RememberedLoad> rules;
    bool rulesChanged = false;
    std::vector<std::array<std::string, 3>> dropped;  // remembered "normal" rules to remove
    // Remembered "separate process" and "disable" choices of this open: applied with the answers
    // in finish(), never at the check, so that a Cancel leaves the engine as it was.
    struct Held {
        std::string id, name, mode;
    };
    std::vector<Held> held;
    std::function<bool(const json::Value&)> proceed;
};

SafeLoad::SafeLoad(ui::Root& root, cmd::Dispatcher& dispatcher, std::filesystem::path diagnostics)
    : root_(root), dispatcher_(dispatcher), diagnostics_(std::move(diagnostics)) {
    plugins = [this](const json::Value& args) {
        return dispatcher_.execute({"project.plugins", args, kOrigin});
    };
    override_ = [this](const std::string& id, const std::string& mode) -> Result<bool> {
        json::Value args = json::Value::object();
        args["plugin"] = id;
        args["mode"] = mode;
        auto r = dispatcher_.execute({"plugin.override", std::move(args), kOrigin});
        if (!r) return r.error();
        // A result without the field is read as a change: setting back is then the safe side.
        const json::Value* changed = r->find("changed");
        return changed && changed->isBool() ? changed->asBool() : true;
    };
    reported = [this] {
        auto found = diagnostics::suspects(diagnostics_);
        return found ? std::move(*found) : std::vector<diagnostics::Suspect>();
    };
    settle = [this](const std::string& incident) -> Status {
        json::Value args = json::Value::object();
        args["incident"] = incident;
        auto r = dispatcher_.execute({"crash.settle", std::move(args), kOrigin});
        if (!r) return r.error();
        return {};
    };
    now = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
}

void SafeLoad::status(const std::string& text) {
    if (onStatus) onStatus(text);
}

void SafeLoad::check(json::Value args, std::function<bool(const json::Value&)> proceed) {
    auto run = std::make_shared<Run>();
    run->proceed = std::move(proceed);
    // Without an engine answer (a bad file, the check unavailable) the load goes on and reports
    // its own error: asking is never what stops an open.
    auto found = plugins ? plugins(args) : Result<json::Value>(Error("safeload.none", "no check"));
    if (!found) {
        (void)run->proceed(json::Value::object());
        return;
    }
    run->found = *found;
    const json::Value* list = run->found.find("plugins");
    if (!list || !list->isArray()) {
        (void)run->proceed(run->found);
        return;
    }
    const std::vector<diagnostics::Suspect> known =
        reported ? reported() : std::vector<diagnostics::Suspect>();
    if (remembered) run->rules = remembered();
    for (const auto& p : list->asArray()) {
        const json::Value* suspect = p.find("suspect");
        if (!suspect || !suspect->isObject() || textAt(p, "mode") != "normal") continue;
        LoadQuestion q;
        q.format = textAt(p, "format");
        q.id = textAt(p, "id");
        const json::Value* resolved = p.find("resolved");
        const json::Value& named = resolved && resolved->isObject() ? *resolved : p;
        q.name = textAt(named, "name").empty() ? q.id : textAt(named, "name");
        q.vendor = textAt(named, "vendor");
        q.version = textAt(named, "version");
        q.build = textAt(*suspect, "build");
        q.stock = flagAt(p, "stock");
        q.bridgeable = flagAt(p, "bridgeable");
        if (const json::Value* inc = suspect->find("incidents"); inc && inc->isArray())
            for (const auto& i : inc->asArray())
                if (i.isString()) q.incidents.push_back(i.asString());
        for (const auto& s : known)
            if (s.format == q.format && s.pluginId == q.id) {
                q.buildId = s.buildId;
                break;
            }
        // A choice remembered for this exact build is applied without asking; a remembered
        // "normal" does not survive a new crash of the build (it is evidence against it).
        bool asked = true;
        if (q.rememberable()) {
            const auto rule =
                std::find_if(run->rules.begin(), run->rules.end(), [&](const auto& r) {
                    return r.format == q.format && r.id == q.id && r.buildId == q.buildId;
                });
            if (rule != run->rules.end()) {
                if (rule->mode == "normal") {
                    // Dropped once the question is answered: a cancelled one changes nothing.
                    run->dropped.push_back({q.format, q.id, q.buildId});
                } else if (!(rule->mode == "bridge" && (!q.bridgeable || q.stock)) && override_) {
                    run->held.push_back({q.id, q.name, rule->mode});
                    asked = false;
                }
            }
        }
        if (asked) run->questions.push_back(std::move(q));
    }
    ask(run);
}

void SafeLoad::ask(std::shared_ptr<Run> run) {
    if (run->answers.size() >= run->questions.size()) return finish(std::move(run));
    asking_ = true;
    const LoadQuestion q = run->questions[run->answers.size()];
    openSafeLoadDialog(
        root_, q,
        [this, run, q](LoadChoice choice, bool remember) {
            run->answers.push_back({q, choice, remember});
            ask(run);
        },
        [this] { asking_ = false; });  // cancelled: the open does not happen
}

Status SafeLoad::enable(const std::string& pluginId, const std::string& format) {
    if (!override_) return {};
    if (auto set = override_(pluginId, "normal"); !set) return set.error();
    // The person's own word that it loads: what a report suspected of it is resolved.
    if (reported && settle)
        for (const auto& suspect : reported())
            if (suspect.pluginId == pluginId && (format.empty() || suspect.format == format))
                for (const auto& incident : suspect.incidents) (void)settle(incident);
    status(pluginId + ": loaded normally again.");
    return {};
}

void SafeLoad::finish(std::shared_ptr<Run> run) {
    asking_ = false;
    // Nothing reaches the engine before every question is answered; then the overrides go first
    // and as a whole: one that fails sets the others back, and nothing is settled or saved.
    struct Pending {
        std::string id, name, mode;
        bool remembered;
    };
    std::vector<Pending> overrides;
    for (const auto& h : run->held) overrides.push_back({h.id, h.name, h.mode, true});
    for (const auto& a : run->answers)
        if (a.choice != LoadChoice::Normal)
            overrides.push_back({a.question.id, a.question.name,
                                 a.choice == LoadChoice::Bridge ? "bridge" : "disable", false});
    // Only what this open changed is set back: an override holds until the app quits, so one
    // that was already in force (a remembered choice applied by an earlier open) is not this
    // open's to undo. The plug-ins asked about were on the normal load, which is what an undo
    // restores.
    std::vector<const Pending*> applied;
    auto undo = [&] {
        for (auto at = applied.rbegin(); at != applied.rend(); ++at)
            if (override_) (void)override_((*at)->id, "normal");
        applied.clear();
    };
    for (const auto& o : overrides) {
        Result<bool> done = override_ ? override_(o.id, o.mode) : Result<bool>(false);
        if (!done) {
            undo();
            status(o.name + ": " + done.error().message);
            return;  // a safety choice that cannot be made is not made halfway: no open
        }
        if (*done) applied.push_back(&o);
    }
    for (const auto& a : run->answers)
        if (a.choice == LoadChoice::Normal) {
            // The person's own word that it loads: the suspicion is resolved (the reports stay).
            // Before the open, so the open does not warn of a plug-in they just cleared.
            for (const auto& incident : a.question.incidents)
                if (settle) (void)settle(incident);
        }
    // The open: when it does not happen the overrides are set back (the choices the person made
    // are not kept for a load that did not take place).
    if (!run->proceed(run->found)) return undo();
    for (const auto& o : overrides)
        if (o.remembered)
            status(o.name + ": " +
                   (o.mode == "bridge" ? "loaded in a separate process" : "not loaded") +
                   ", as you chose before. Edit > Preferences lists it.");
    for (const auto& d : run->dropped) {
        run->rulesChanged |=
            std::erase_if(run->rules, [&](const auto& r) {
                return r.format == d[0] && r.id == d[1] && r.buildId == d[2] && r.mode == "normal";
            }) > 0;
    }
    for (const auto& a : run->answers) {
        if (!a.remember || !a.question.rememberable()) continue;
        RememberedLoad rule;
        rule.format = a.question.format;
        rule.id = a.question.id;
        rule.buildId = a.question.buildId;
        rule.mode = a.choice == LoadChoice::Bridge    ? "bridge"
                    : a.choice == LoadChoice::Disable ? "disable"
                                                      : "normal";
        rule.name = a.question.name;
        rule.vendor = a.question.vendor;
        rule.version = a.question.version;
        rule.rememberedMs = now ? now() : 0;
        std::erase_if(run->rules, [&](const auto& r) {
            return r.format == rule.format && r.id == rule.id && r.buildId == rule.buildId;
        });
        run->rules.push_back(std::move(rule));
        run->rulesChanged = true;
    }
    if (run->rulesChanged && setRemembered) {
        if (const Status saved = setRemembered(run->rules); !saved)
            status("Could not keep your choices: " + saved.error().message);
    }
}

}  // namespace supple::gui
