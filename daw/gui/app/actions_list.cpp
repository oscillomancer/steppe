// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "actions_list.h"
#include "supple/ui/shortcut.h"

#include <SDL3/SDL_keycode.h>

#include <memory>

#include "chrome.h"
#include "learn_dialog.h"
#include "supple/engine/midi_control.h"
#include "supple/json/parse.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

// Captures the next chord (any key, printable or not) for a binding.
class KeyCapture : public ui::Node {
  public:
    explicit KeyCapture(std::function<void(std::optional<actions::Chord>)> done)
        : Node("actions.capture", ui::Role::Custom), done_(std::move(done)) {
        focusable = true;
        layout.height = ui::SizeRule::fixed(0);
    }
    bool onKey(const ui::KeyEvent& e) override {
        switch (e.key) {
            case SDLK_LCTRL:
            case SDLK_RCTRL:
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
            case SDLK_LALT:
            case SDLK_RALT:
            case SDLK_LGUI:
            case SDLK_RGUI:
                return true;  // wait for the key itself
            case SDLK_ESCAPE:
                done_(std::nullopt);
                return true;
            default:
                done_(actions::Chord{e.key, e.mods});
                return true;
        }
    }
    bool onText(std::string_view) override { return true; }  // swallow the typed character

  private:
    std::function<void(std::optional<actions::Chord>)> done_;
};

class ActionsPanel : public ui::Node {
  public:
    ActionsPanel(ui::Root& root, AppActions& actions, Chrome* chrome)
        : Node("actions"), root_(root), actions_(actions), chrome_(chrome) {
        layout.dir = ui::Layout::Dir::Column;
        layout.width = ui::SizeRule::fixed(600);
        layout.height = ui::SizeRule::fixed(440);
        layout.padding = ui::Insets::all(10);
        layout.gap = 8;
        search_ = &emplace<ui::TextField>("actions.search", "", "Search actions…");
        list_ = &emplace<ui::List>("actions.results");
        capture_ = &emplace<KeyCapture>([this](std::optional<actions::Chord> c) { captured(c); });
        auto& footer = emplace<ui::Node>("actions.footer");
        footer.layout.dir = ui::Layout::Dir::Row;
        footer.layout.height = ui::SizeRule::fixed(24);
        footer.layout.gap = 8;
        footer.layout.align = ui::Layout::Align::Center;
        hint_ = &footer.emplace<ui::Label>("actions.hint",
                                           ui::shortcutLabel(chrome_ ? kChromeHint : kHint),
                                           ui::Font::Ui, "caption", "ink-quiet");
        hint_->layout.width = ui::SizeRule::fraction();
        footer.emplace<ui::Button>("actions.keys.reset", "Reset keys\u2026", [this] {
            openKeymapReset(root_, actions_, [this] {
                hint_->setText("Key bindings reset to the defaults");
                list_->invalidate();
                root_.focus(search_);
            });
        });
        search_->onChange = [this](const std::string& q) { filter(q); };
        search_->onSubmit = [this](const std::string&) { activate(); };
        search_->onOtherKey = [this](const ui::KeyEvent& e) {
            // Up/Down/PgUp/PgDn/Home/End move the selection.
            return shortcut(e) || list_->onKey(e);
        };
        list_->rowHeight = 30;
        list_->onActivate = [this](std::size_t) { activate(); };
        list_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
            const ui::Theme& t = c.theme();
            const actions::Action& a = *matches_[i];
            // Keys in the mono face; a MIDI control (its "·" needs the UI face) after them.
            const std::string keys = actions_.keysFor(a.id, true);
            const std::string midi = actions_.controlsFor(a.id);
            const float midiW =
                midi.empty() ? 0 : c.textWidth(midi, ui::Font::Ui, t.type("caption")) + 12;
            const float keysW =
                (keys.empty() ? 0 : c.textWidth(keys, ui::Font::Mono, t.type("caption")) + 16) +
                midiW;
            const float nameW = c.textWidth(a.name, ui::Font::Ui, t.type("small"));
            c.text({r.x + 10, r.y, r.w - 20 - keysW, r.h}, a.name, ui::Font::Ui, t.type("small"),
                   t.color("ink"));
            if (!a.command.empty() && a.command != a.name)
                c.text({r.x + 22 + nameW, r.y, std::max(0.0f, r.w - 34 - nameW - keysW), r.h}, a.id,
                       ui::Font::Mono, t.type("caption"), t.color("ink-faint"));
            if (!keys.empty())
                c.text({r.x, r.y, r.w - 10 - midiW, r.h}, keys, ui::Font::Mono, t.type("caption"),
                       t.color("ink-quiet"), ui::kRight | ui::kMiddle);
            if (!midi.empty())
                c.text({r.x, r.y, r.w - 10, r.h}, midi, ui::Font::Ui, t.type("caption"),
                       t.color("accent"), ui::kRight | ui::kMiddle);
        };
        filter("");
    }

    void paint(ui::Canvas&) override {}
    // The panel's keys work wherever its focus is (the search field, or the
    // list after a click on a row); typing goes on searching.
    bool onKey(const ui::KeyEvent& e) override { return shortcut(e); }
    bool onText(std::string_view text) override {
        root_.focus(search_);
        return search_->onText(text);
    }

  private:
    bool shortcut(const ui::KeyEvent& e) {
        if (!(e.mods & ui::kCtrl)) return false;
        if (e.key == SDLK_B) {
            startCapture();
            return true;
        }
        if (e.key == SDLK_L) {
            learn();
            return true;
        }
        if (e.key == SDLK_T && chrome_) {
            toggleToolbar();
            return true;
        }
        if (e.key == SDLK_M && chrome_) {
            openMenuChoice();
            return true;
        }
        return false;
    }

    static constexpr const char* kHint =
        "Enter run · Ctrl+B bind a key · Ctrl+L MIDI learn · Esc close";
    static constexpr const char* kChromeHint =
        "Enter run · Ctrl+B key · Ctrl+L MIDI learn · Ctrl+T toolbar · Ctrl+M menu · Esc close";

    // MIDI learn for the selected action: a controller runs it (a press) or,
    // for a "(MIDI CC)" action, drives it.
    void learn() {
        const actions::Action* a = current();
        if (!a) return;
        const std::string id = a->id;
        LearnRequest request;
        request.title = a->name;
        request.pressOnly = !a->control;
        request.takeover = a->takeover;
        if (const std::string follow = actions_.followingControl(id); !follow.empty())
            if (const actions::Action* f = actions_.registry().find(follow))
                request.alternative = f->name;
        if (const auto bound = actions_.controls().bindingsFor(id); !bound.empty())
            request.current =
                LearnChoice{bound.front().control, bound.front().mode, bound.front().softTakeover};
        request.onOk = [this, id, takeover = a->takeover](const LearnChoice& choice) {
            const std::string had = actions_.controls().bind(
                {id, choice.control, choice.mode, takeover && choice.softTakeover});
            std::string note = engine::describe(choice.control) + " → " + id;
            if (!had.empty()) note += " (was " + had + ")";
            if (auto saved = actions_.saveControls(); !saved)
                note += " (not saved: " + saved.error().message + ")";
            hint_->setText(note);
            list_->invalidate();
            root_.focus(search_);
        };
        request.onRemove = [this, id] {
            actions_.controls().unbindAction(id);
            std::string note = "No MIDI control for " + id;
            if (auto saved = actions_.saveControls(); !saved)
                note += " (not saved: " + saved.error().message + ")";
            hint_->setText(note);
            list_->invalidate();
            root_.focus(search_);
        };
        openLearnDialog(root_, actions_, actions_.app(), std::move(request));
    }

    void toggleToolbar() {
        const actions::Action* a = current();
        if (!a) return;
        const std::string id = a->id;
        chrome_->toggleToolbar(id);
        hint_->setText((chrome_->inToolbar(id) ? "On the toolbar: " : "Off the toolbar: ") + id);
    }

    // Which menu holds the action: a checked entry per menu; choosing one
    // adds the action there or takes it out.
    void openMenuChoice() {
        const actions::Action* a = current();
        if (!a) return;
        const std::string id = a->id;
        std::vector<ui::MenuItem> items;
        for (const ChromeLayout::Menu& m : chrome_->layout().menus) {
            ui::MenuItem item;
            item.label = m.title;
            item.checked = chrome_->inMenu(m.title, id);
            item.run = [this, title = m.title, id] {
                chrome_->toggleMenu(title, id);
                hint_->setText((chrome_->inMenu(title, id) ? "In " : "Out of ") + title + ": " +
                               id);
            };
            items.push_back(std::move(item));
        }
        root_.openPopup(ui::makeMenu("actions.menus", std::move(items),
                                     [this] {
                                         root_.closePopup();
                                         root_.focus(search_);
                                     }),
                        {}, ui::Placement::Center);
    }

    void filter(const std::string& q) {
        matches_.clear();
        if (q.empty()) {
            for (const actions::Action* a : actions_.registry().all()) matches_.push_back(a);
            std::sort(matches_.begin(), matches_.end(),
                      [](const actions::Action* x, const actions::Action* y) {
                          return x->name < y->name;
                      });
        } else {
            for (const auto& m : actions_.registry().search(q, 200)) matches_.push_back(m.action);
        }
        list_->setCount(matches_.size());
        list_->select(matches_.empty() ? std::nullopt : std::optional<std::size_t>(0));
    }

    const actions::Action* current() const {
        const auto i = list_->selected();
        return i && *i < matches_.size() ? matches_[*i] : nullptr;
    }

    void activate() {
        const actions::Action* a = current();
        if (!a) return;
        ui::Root& root = root_;
        AppActions& actions = actions_;
        const actions::Action action = *a;  // the panel is destroyed by closePopup
        root.closePopup();
        if (action.prompts.empty()) {
            (void)actions.run(action);
            return;
        }
        openActionPrompt(root, actions, action);
    }

    void startCapture() {
        if (!current()) return;
        capturing_ = current()->id;
        hint_->setText("Press the keys for \"" + current()->name + "\" (Esc cancels)");
        root_.focus(capture_);
    }

    void captured(std::optional<actions::Chord> chord) {
        root_.focus(search_);
        if (!chord) {
            hint_->setText(ui::shortcutLabel(chrome_ ? kChromeHint : kHint));
            return;
        }
        actions::Binding b{{*chord}, "global", capturing_};
        // The main window looks in "arrange" before "global": a default there
        // on the same keys would hide the new binding, so it gives way.
        std::string replaced;
        for (const actions::Binding& other : actions_.keymap().bindings())
            if (other.keys == b.keys && other.action != capturing_ &&
                (other.section == "arrange" || other.section == "global"))
                replaced = other.action;
        const std::vector<actions::Binding> all = actions_.keymap().bindings();
        for (const actions::Binding& other : all)
            if (other.keys == b.keys && other.section == "arrange") actions_.keymap().unbind(other);
        actions_.keymap().bind(b);
        std::string note = actions::describe(*chord) + " → " + capturing_;
        if (!replaced.empty()) note += " (was " + replaced + ")";
        if (auto saved = actions_.keymap().save(actions_.keymapFile()); !saved)
            note += " (not saved: " + saved.error().message + ")";
        for (const auto& [x, y] : actions_.keymap().conflicts())
            if (x == b || y == b) note += " — conflicts with " + (x == b ? y.action : x.action);
        hint_->setText(note);
        list_->invalidate();
    }

    ui::Root& root_;
    AppActions& actions_;
    Chrome* chrome_ = nullptr;
    ui::TextField* search_ = nullptr;
    ui::List* list_ = nullptr;
    ui::Node* capture_ = nullptr;
    ui::Label* hint_ = nullptr;
    std::vector<const actions::Action*> matches_;
    std::string capturing_;
};

}  // namespace

void openActionPrompt(ui::Root& root, AppActions& actions, const actions::Action& action) {
    auto form = std::make_unique<ui::Node>("actions.prompt");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(420);
    form->layout.height =
        ui::SizeRule::fixed(64.0f + 56.0f * static_cast<float>(action.prompts.size()));
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 6;
    form->emplace<ui::Label>("actions.prompt.title", action.name, ui::Font::UiBold, "body");
    auto fields = std::make_shared<std::vector<ui::TextField*>>();
    for (const std::string& p : action.prompts) {
        form->emplace<ui::Label>("actions.prompt.label." + p, p, ui::Font::Mono, "caption",
                                 "ink-quiet");
        fields->push_back(&form->emplace<ui::TextField>("actions.prompt." + p));
    }
    auto submit = [rootp = &root, actionsp = &actions, action, fields](const std::string&) {
        json::Value values = json::Value::object();
        for (std::size_t i = 0; i < fields->size(); ++i) {
            const std::string& text = (*fields)[i]->text();
            auto parsed = json::parse(text);  // numbers, booleans, arrays; else the text itself
            values[action.prompts[i]] = parsed ? *parsed : json::Value(text);
        }
        // closePopup destroys this lambda (it lives in the form): copy first.
        ui::Root* owner = rootp;
        AppActions* runner = actionsp;
        const actions::Action act = action;
        owner->closePopup();
        (void)runner->run(act, values);
    };
    for (ui::TextField* f : *fields) f->onSubmit = submit;
    root.openPopup(std::move(form), {}, ui::Placement::Center);
}

void openKeymapReset(ui::Root& root, AppActions& actions, std::function<void()> done) {
    auto form = std::make_unique<ui::Node>("keys.reset");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(420);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(14);
    form->layout.gap = 10;
    form->emplace<ui::Label>("keys.reset.title", "Reset every key binding to the defaults?",
                             ui::Font::UiBold, "body");
    form->emplace<ui::Label>("keys.reset.text", "Your own bindings and removals are lost.",
                             ui::Font::Ui, "small", "ink-quiet");
    auto& row = form->emplace<ui::Node>("keys.reset.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(28);
    row.layout.gap = 8;
    row.emplace<ui::Button>("keys.reset.confirm", "Reset", [&root, &actions, done] {
        auto after = done;  // closePopup destroys this button and its closure
        root.closePopup();
        if (auto reset = actions.resetKeymap(); !reset && actions.onError)
            actions.onError("keys: " + reset.error().message);
        if (after) after();
    });
    row.emplace<ui::Button>("keys.reset.cancel", "Cancel", [&root] { root.closePopup(); });
    root.openPopup(std::move(form), {}, ui::Placement::Center);
}

void openActionsList(ui::Root& root, AppActions& actions, Chrome* chrome) {
    if (root.popupOpen()) root.closePopups();
    root.openPopup(std::make_unique<ActionsPanel>(root, actions, chrome), {},
                   ui::Placement::Center);
}

}  // namespace supple::gui
