// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_window.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
constexpr float kBrowserW = 220, kHeaderH = 34, kStatusH = 22;
constexpr auto kTelemetryInterval = std::chrono::milliseconds(33);

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string str(const json::Value& v, const char* key) {
    const json::Value* x = v.find(key);
    return x && x->isString() ? x->asString() : std::string();
}

}  // namespace

// --- the tabs ------------------------------------------------------------------------

class RackTabs : public ui::Node {
  public:
    struct Tab {
        model::Id rack;
        std::string label;
        std::optional<ui::Color> color;  // the home channel's
    };
    struct Hooks {
        std::function<void(model::Id)> select;
        std::function<void(model::Id)> rename;
        std::function<void(model::Id, ui::Point)> menu;
        std::function<void()> add;
    };
    explicit RackTabs(Hooks hooks) : Node("rack.tabs", ui::Role::Tab), hooks_(std::move(hooks)) {
        interactive = true;
        layout.width = ui::SizeRule::fraction();
        layout.height = ui::SizeRule::fixed(kHeaderH);
    }
    void set(std::vector<Tab> tabs, std::optional<model::Id> current) {
        tabs_ = std::move(tabs);
        current_ = current;
        invalidate();
    }
    std::string valueText() const override {
        std::string out;
        for (const auto& t : tabs_)
            out +=
                (out.empty() ? "" : " | ") + std::string(current_ == t.rack ? "*" : "") + t.label;
        return out;
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        boxes_.clear();
        float x = b.x + 6;
        for (const auto& tab : tabs_) {
            const float w =
                std::clamp(c.textWidth(tab.label, ui::Font::UiBold, 12) + 34, 76.0f, 220.0f);
            const ui::Rect r{x, b.y + 6, w, b.h - 6};
            boxes_.push_back(r);
            const bool on = current_ == tab.rack, over = hoverAt_ && r.contains(*hoverAt_);
            if (on) {
                c.fillRound({r.x, r.y, r.w, r.h + 6}, 5, t.color("ground"));
            } else if (over) {
                c.fillRound({r.x, r.y + 2, r.w, r.h - 4}, 5, t.color("raised"));
            }
            const ui::Color dot = tab.color.value_or(t.color("channel"));
            c.fillCircle({r.x + 13, r.y + r.h / 2}, 3.5f, on ? dot : dot.withAlpha(0.6f));
            c.text({r.x + 22, r.y, r.w - 28, r.h}, tab.label, on ? ui::Font::UiBold : ui::Font::Ui,
                   12, on ? t.color("ink") : t.color("ink-quiet"));
            x += w + 2;
        }
        plus_ = {x + 2, b.y + 8, 26, b.h - 12};
        const bool over = hoverAt_ && plus_.contains(*hoverAt_);
        if (over) c.fillRound(plus_, 4, t.color("raised"));
        const ui::Point m{plus_.x + plus_.w / 2, plus_.y + plus_.h / 2};
        const ui::Color ink = over ? t.color("ink") : t.color("ink-quiet");
        c.line({m.x - 5, m.y}, {m.x + 5, m.y}, ink, 1.5f);
        c.line({m.x, m.y - 5}, {m.x, m.y + 5}, ink, 1.5f);
    }
    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        if (e.type == T::Move || e.type == T::Enter) {
            hoverAt_ = e.pos;
            invalidate();
            return true;
        }
        if (e.type == T::Leave) {
            hoverAt_.reset();
            invalidate();
            return true;
        }
        if (e.type != T::Down) return false;
        if (plus_.contains(e.pos) && e.button == 1) {
            if (hooks_.add) hooks_.add();
            return true;
        }
        for (std::size_t i = 0; i < boxes_.size() && i < tabs_.size(); ++i) {
            if (!boxes_[i].contains(e.pos)) continue;
            const model::Id id = tabs_[i].rack;
            if (e.button == 3) {
                if (hooks_.select) hooks_.select(id);
                if (hooks_.menu) hooks_.menu(id, e.pos);
            } else if (e.button == 1) {
                if (hooks_.select) hooks_.select(id);
                if (e.clicks >= 2 && hooks_.rename) hooks_.rename(id);
            }
            return true;
        }
        return true;
    }
    std::optional<ui::Rect> tabRect(model::Id id) const {
        for (std::size_t i = 0; i < boxes_.size() && i < tabs_.size(); ++i)
            if (tabs_[i].rack == id) return boxes_[i];
        return std::nullopt;
    }
    ui::Rect plusRect() const { return plus_; }

  private:
    Hooks hooks_;
    std::vector<Tab> tabs_;
    std::optional<model::Id> current_;
    std::vector<ui::Rect> boxes_;
    ui::Rect plus_;
    std::optional<ui::Point> hoverAt_;
};

// --- the browser ---------------------------------------------------------------------

class RackBrowser : public ui::Node {
  public:
    struct Entry {
        std::string id, name, description;
        std::vector<std::string> tags;
        rack::Origin origin = rack::Origin::Stock;
    };
    struct Hooks {
        std::function<void(const std::string& type)> add;
        // A row dragged out over the window (none: back over the list or done).
        std::function<void(const std::string& name, std::optional<ui::Point> at)> dragOver;
        std::function<void(const std::string& type, ui::Point at)> drop;
    };
    explicit RackBrowser(Hooks hooks) : Node("rack.browser"), hooks_(std::move(hooks)) {
        layout.dir = ui::Layout::Dir::Column;
        layout.width = ui::SizeRule::fixed(kBrowserW);
        layout.padding = {10, 10, 10, 6};
        layout.gap = 8;
        search_ = &emplace<ui::TextField>("rack.browser.search", "", "Search modules");
        search_->layout.height = ui::SizeRule::fixed(26);
        search_->onChange = [this](const std::string&) { filter(); };
        search_->onOtherKey = [this](const ui::KeyEvent& e) { return list_->key(e); };
        search_->onSubmit = [this](const std::string&) { list_->activate(); };
        tags_ = &emplace<Tags>(*this);
        list_ = &emplace<List>(*this);
    }
    void setEntries(std::vector<Entry> entries) {
        entries_ = std::move(entries);
        std::set<std::string> tags;
        for (const auto& e : entries_) tags.insert(e.tags.begin(), e.tags.end());
        allTags_.assign(tags.begin(), tags.end());
        if (tag_ && !tags.contains(*tag_)) tag_.reset();
        filter();
        tags_->relayout();
    }
    std::vector<std::string> shown() const {
        std::vector<std::string> out;
        for (const auto& r : rows_)
            if (r.entry) out.push_back(entries_[*r.entry].id);
        return out;
    }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().right() - 1, bounds().y, 1, bounds().h}, c.theme().color("rule"));
    }
    void setTag(std::optional<std::string> tag) {
        tag_ = std::move(tag);
        filter();
        tags_->invalidate();
    }

  private:
    struct Row {
        std::optional<std::size_t> entry;  // none: an origin's heading
        std::string heading;
    };
    void filter() {
        rows_.clear();
        std::vector<std::string> words;
        {
            std::string q = lower(search_->text()), w;
            for (char ch : q + " ")
                if (ch == ' ') {
                    if (!w.empty()) words.push_back(w);
                    w.clear();
                } else {
                    w += ch;
                }
        }
        std::optional<rack::Origin> origin;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (tag_ && std::find(e.tags.begin(), e.tags.end(), *tag_) == e.tags.end()) continue;
            std::string hay = lower(e.name + " " + e.id + " " + e.description);
            for (const auto& t : e.tags) hay += " " + t;
            if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) {
                    return hay.find(w) != std::string::npos;
                }))
                continue;
            if (origin != e.origin) {
                origin = e.origin;
                const auto name = rack::originName(e.origin);
                rows_.push_back({std::nullopt, std::string(name)});
            }
            rows_.push_back({i, {}});
        }
        list_->reset();
    }

    class Tags : public ui::Node {
      public:
        explicit Tags(RackBrowser& b) : Node("rack.browser.tags"), b_(b) {
            interactive = true;
            layout.height = ui::SizeRule::content();
        }
        ui::Size measure(const ui::Canvas& c, ui::Size available) override {
            return {available.w, flow(c, available.w, nullptr)};
        }
        void paint(ui::Canvas& c) override {
            boxes_.clear();
            flow(c, bounds().w, &c);
        }
        bool onPointer(const ui::PointerEvent& e) override {
            if (e.type != ui::PointerEvent::Type::Down || e.button != 1)
                return e.type != ui::PointerEvent::Type::Move;
            for (const auto& [r, tag] : boxes_)
                if (r.contains(e.pos)) {
                    b_.setTag(b_.tag_ == tag ? std::nullopt : std::optional(tag));
                    return true;
                }
            return true;
        }

      private:
        // Lays out (and with `c` paints) the chips; returns their height.
        float flow(const ui::Canvas& c, float width, ui::Canvas* paint) {
            float x = 0, y = 0;
            constexpr float h = 18, gap = 4;
            for (const auto& tag : b_.allTags_) {
                const float w = c.textWidth(tag, ui::Font::Ui, 10.5f) + 14;
                if (x > 0 && x + w > width) {
                    x = 0;
                    y += h + gap;
                }
                if (paint) {
                    const ui::Theme& t = paint->theme();
                    const ui::Rect r{bounds().x + x, bounds().y + y, w, h};
                    const bool on = b_.tag_ == tag;
                    paint->fillRound(r, h / 2, on ? t.color("accent") : t.color("raised"));
                    paint->text(r, tag, ui::Font::Ui, 10.5f,
                                on ? t.color("ground") : t.color("ink-quiet"),
                                ui::kCenter | ui::kMiddle);
                    boxes_.emplace_back(r, tag);
                }
                x += w + gap;
            }
            return b_.allTags_.empty() ? 0 : y + h;
        }
        RackBrowser& b_;
        std::vector<std::pair<ui::Rect, std::string>> boxes_;
    };

    class List : public ui::Node {
      public:
        explicit List(RackBrowser& b) : Node("rack.browser.list", ui::Role::List), b_(b) {
            interactive = true;
            clipsChildren = true;
            layout.height = ui::SizeRule::fraction();
        }
        void reset() {
            scroll_ = 0;
            selected_.reset();
            for (std::size_t i = 0; i < b_.rows_.size(); ++i)
                if (b_.rows_[i].entry) {
                    if (!b_.search_->text().empty() || b_.tag_) selected_ = i;
                    break;
                }
            invalidate();
        }
        std::string valueText() const override {
            return std::to_string(b_.shown().size()) + " modules";
        }
        bool key(const ui::KeyEvent& e) {
            if (e.key != SDLK_DOWN && e.key != SDLK_UP) return false;
            const int dir = e.key == SDLK_DOWN ? 1 : -1;
            long i = selected_ ? static_cast<long>(*selected_) : -1;
            for (i += dir; i >= 0 && i < static_cast<long>(b_.rows_.size()); i += dir)
                if (b_.rows_[static_cast<std::size_t>(i)].entry) {
                    selected_ = static_cast<std::size_t>(i);
                    break;
                }
            invalidate();
            return true;
        }
        void activate() {
            if (selected_ && *selected_ < b_.rows_.size() && b_.rows_[*selected_].entry &&
                b_.hooks_.add)
                b_.hooks_.add(b_.entries_[*b_.rows_[*selected_].entry].id);
        }
        void paint(ui::Canvas& c) override {
            const ui::Theme& t = c.theme();
            const ui::Rect b = bounds();
            c.save();
            c.clip(b);
            float y = b.y - scroll_;
            for (std::size_t i = 0; i < b_.rows_.size(); ++i) {
                const Row& r = b_.rows_[i];
                const float h = rowH(r);
                if (y + h >= b.y && y <= b.bottom()) {
                    if (!r.entry) {
                        std::string head = r.heading;
                        for (char& ch : head)
                            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        c.text({b.x + 2, y + 6, b.w - 4, h - 6}, head, ui::Font::UiBold, 9.5f,
                               t.color("ink-faint"), ui::kLeft | ui::kMiddle);
                    } else {
                        const Entry& e = b_.entries_[*r.entry];
                        const ui::Rect box{b.x - 4, y + 1, b.w + 8, h - 2};
                        if (selected_ == i)
                            c.fillRound(box, 4, t.color("selection"));
                        else if (hover_ == i)
                            c.fillRound(box, 4, t.color("raised"));
                        c.text({b.x + 4, y + 3, b.w - 8, 16}, e.name, ui::Font::UiBold, 12,
                               t.color("ink"));
                        c.text({b.x + 4, y + 19, b.w - 8, 14}, e.description, ui::Font::Ui, 10.5f,
                               t.color("ink-quiet"));
                    }
                }
                y += h;
            }
            if (b_.rows_.empty())
                c.text({b.x, b.y + 8, b.w, 16}, "No module matches", ui::Font::Ui, 11,
                       t.color("ink-faint"), ui::kCenter | ui::kMiddle);
            c.restore();
        }
        bool onPointer(const ui::PointerEvent& e) override {
            using T = ui::PointerEvent::Type;
            const ui::Rect b = bounds();
            if (e.type == T::Wheel) {
                float total = 0;
                for (const auto& r : b_.rows_) total += rowH(r);
                scroll_ = std::clamp(scroll_ - e.wheelY * 36, 0.0f, std::max(0.0f, total - b.h));
                invalidate();
                return true;
            }
            if (e.type == T::Leave) {
                hover_.reset();
                invalidate();
                return true;
            }
            if (e.type == T::Move || e.type == T::Enter) {
                if (press_) {
                    const bool out = !b.contains(e.pos);
                    if (std::hypot(e.pos.x - pressAt_.x, e.pos.y - pressAt_.y) > 4)
                        dragging_ = true;
                    if (dragging_ && b_.hooks_.dragOver)
                        b_.hooks_.dragOver(b_.entries_[*b_.rows_[*press_].entry].name,
                                           out ? std::optional(e.pos) : std::nullopt);
                    return true;
                }
                const auto row = rowAt(e.pos);
                if (row != hover_) {
                    hover_ = row;
                    invalidate();
                }
                return true;
            }
            if (e.type == T::Down && e.button == 1) {
                const auto row = rowAt(e.pos);
                if (!row || !b_.rows_[*row].entry) return true;
                selected_ = row;
                invalidate();
                if (e.clicks >= 2) {
                    activate();
                    return true;
                }
                press_ = row;
                pressAt_ = e.pos;
                dragging_ = false;
                return true;
            }
            if (e.type == T::Up || e.type == T::Cancel) {
                const auto row = std::exchange(press_, std::nullopt);
                const bool dropped =
                    std::exchange(dragging_, false) && row && e.type == T::Up && !b.contains(e.pos);
                if (b_.hooks_.dragOver) b_.hooks_.dragOver({}, std::nullopt);
                if (dropped && b_.hooks_.drop)
                    b_.hooks_.drop(b_.entries_[*b_.rows_[*row].entry].id, e.pos);
                return true;
            }
            return false;
        }

      private:
        static float rowH(const Row& r) { return r.entry ? 36.0f : 26.0f; }
        std::optional<std::size_t> rowAt(ui::Point p) const {
            float y = bounds().y - scroll_;
            for (std::size_t i = 0; i < b_.rows_.size(); ++i) {
                const float h = rowH(b_.rows_[i]);
                if (p.y >= y && p.y < y + h) return i;
                y += h;
            }
            return std::nullopt;
        }
        RackBrowser& b_;
        float scroll_ = 0;
        std::optional<std::size_t> selected_, hover_, press_;
        ui::Point pressAt_;
        bool dragging_ = false;
    };

    Hooks hooks_;
    std::vector<Entry> entries_;
    std::vector<Row> rows_;
    std::vector<std::string> allTags_;
    std::optional<std::string> tag_;
    ui::TextField* search_ = nullptr;
    Tags* tags_ = nullptr;
    List* list_ = nullptr;
};

namespace {

// The bar under the space: what the rack holds, or what went wrong.
class StatusBar : public ui::Node {
  public:
    StatusBar() : Node("rack.statusbar") {
        layout.dir = ui::Layout::Dir::Row;
        layout.height = ui::SizeRule::fixed(kStatusH);
        layout.padding = ui::Insets::xy(10, 0);
        layout.align = ui::Layout::Align::Center;
    }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().y, bounds().w, 1}, c.theme().color("rule"));
    }
};

class Header : public ui::Node {
  public:
    Header() : Node("rack.header") {
        layout.dir = ui::Layout::Dir::Row;
        layout.height = ui::SizeRule::fixed(kHeaderH);
        layout.align = ui::Layout::Align::Center;
        layout.padding = {6, 0, 8, 0};
        layout.gap = 4;
    }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
    }
};

}  // namespace

// --- the window ----------------------------------------------------------------------

RackWindow::RackWindow(FloatingPanel& panel, AppActions& actions, cmd::Dispatcher& dispatcher,
                       engine::EngineApp& app, UiState& ui)
    : panel_(panel), actions_(actions), dispatcher_(dispatcher), app_(app), ui_(ui) {
    actions_.addUi("ui.rack", "View: racks", [this] {
        if (isOpen())
            close();
        else
            open();
    });
    actions_.addUiFor("ui.rack.show", "Track: show rack", "track",
                      [this](const json::Value& v) -> Status {
                          // The first rack homed on the track (or the selected one).
                          std::string text = v.isString() ? v.asString() : std::string();
                          if (text.empty() && !ui_.selectedTracks.empty())
                              text = ui_.selectedTracks.front();
                          const auto track = model::Id::parse(text);
                          if (!track)
                              return Error("gui.rack", "no track given and nothing is selected",
                                           "/track", "select a channel first");
                          for (const auto& e : app_.head().racks)
                              if (e.value->track == *track) {
                                  open(e.id);
                                  return {};
                              }
                          return Error("gui.rack", "the track has no rack", "/track",
                                       "add one with Track: add rack");
                      });
    actions_.addUiFor(
        "ui.rack.add", "Track: add rack", "track", [this](const json::Value& v) -> Status {
            const auto track = v.isString() ? model::Id::parse(v.asString()) : std::nullopt;
            return addRack(track);
        });
}

RackWindow::~RackWindow() { close(); }

Result<json::Value> RackWindow::exec(const std::string& command, json::Value args,
                                     const std::string& gesture) {
    cmd::Origin origin = kOrigin;
    origin.gesture = gesture;
    auto r = dispatcher_.execute({command, std::move(args), origin});
    if (!r) report(r.error());
    return r;
}

void RackWindow::report(const Error& e) {
    error_ = e.message + (e.hint.empty() ? "" : " (" + e.hint + ")");
    if (status_) status_->setText(status());
    if (!isOpen() && actions_.onError) actions_.onError("rack: " + error_);
}

std::string RackWindow::status() const {
    if (!error_.empty()) return error_;
    if (!rack_ || !space_) return "No racks";
    const auto& s = space_->scene();
    std::string out =
        model::rackLabel(app_.head(), *rack_) + "  ·  " + std::to_string(s.modules.size()) +
        (s.modules.size() == 1 ? " module" : " modules") + "  ·  " +
        std::to_string(s.cables.size()) + (s.cables.size() == 1 ? " cable" : " cables");
    if (!space_->selection().empty())
        out += "  ·  " + std::to_string(space_->selection().size()) + " selected";
    return out;
}

void RackWindow::open(std::optional<model::Id> rack) {
    if (rack) rack_ = rack;
    if (isOpen()) {
        show(rack_);
        panel_.raise();
        return;
    }
    if (auto ready = panel_.ensure(); !ready) {
        if (actions_.onError) actions_.onError(ready.error().message);
        return;
    }
    panel_.setTitle("Racks");
    auto content = std::make_unique<ui::Node>("rack");
    content->layout.dir = ui::Layout::Dir::Column;
    auto& header = content->emplace<Header>();
    browserToggle_ = &header.emplace<ui::Button>("rack.browser.toggle", "Modules", [this] {
        browserVisible_ = !browserVisible_;
        if (browserColumn_) {
            browserColumn_->visible = browserVisible_;
            browserColumn_->relayout();
        }
    });
    RackTabs::Hooks th;
    th.select = [this](model::Id id) { show(id); };
    th.rename = [this](model::Id id) { renameRack(id); };
    th.menu = [this](model::Id id, ui::Point at) { tabMenu(id, at); };
    th.add = [this] { (void)addRack(); };
    tabs_ = &header.emplace<RackTabs>(std::move(th));
    zoom_ = &header.emplace<ui::Button>("rack.zoom", "100 %", [this] {
        if (!space_) return;
        if (std::fabs(space_->view().zoom - 1) < 1e-6)
            space_->frameAll();
        else {
            const ui::Rect b = space_->bounds();
            space_->zoomAt({b.x + b.w / 2, b.y + b.h / 2}, 1 / space_->view().zoom);
        }
    });
    auto& body = content->emplace<ui::Node>("rack.body");
    body.layout.dir = ui::Layout::Dir::Row;
    RackBrowser::Hooks bh;
    bh.add = [this](const std::string& type) { (void)addModule(type); };
    bh.dragOver = [this](const std::string& name, std::optional<ui::Point> at) {
        if (space_)
            space_->dragTypeOver(name, at && space_->bounds().contains(*at) ? at : std::nullopt);
    };
    bh.drop = [this](const std::string& type, ui::Point at) {
        if (space_ && space_->bounds().contains(at)) (void)addModule(type, space_->toSpace(at));
    };
    browser_ = &body.emplace<RackBrowser>(std::move(bh));
    browserColumn_ = browser_;
    browserColumn_->visible = browserVisible_;
    RackSpace::Hooks sh;
    sh.move = [this](const std::vector<RackSpace::Move>& moves) { move(moves); };
    sh.open = [this](model::Id module) {
        const model::RackModule* m = app_.head().modules.find(module);
        if (!m || !m->fx) return;
        if (!openEditor) return;
        if (auto r = openEditor(*m->fx); !r) report(r.error());
    };
    sh.moduleMenu = [this](model::Id module, ui::Point at) { moduleMenu(module, at); };
    sh.spaceMenu = [this](ui::Point at, ui::Point sp) { spaceMenu(at, sp); };
    sh.param = [this](model::Id module, int index, double value, ui::Gesture g) {
        param(module, index, value, g);
    };
    sh.paramMenu = [this](model::Id module, int index, ui::Point at) {
        paramMenu(module, index, at);
    };
    sh.connect = [this](const model::CableEnd& from, const model::CableEnd& to,
                        std::optional<model::Id> replace) { connect(from, to, replace); };
    sh.disconnect = [this](model::Id cable) {
        json::Value args = json::Value::object();
        args["cable"] = cable.str();
        (void)exec("cable.remove", std::move(args));
    };
    sh.amount = [this](model::Id cable, double amount, ui::Gesture g) {
        if (g == ui::Gesture::Begin) ++gesture_;
        if (g != ui::Gesture::Move) return;
        json::Value args = json::Value::object();
        args["cable"] = cable.str();
        args["amount"] = amount;
        (void)exec("cable.set", std::move(args),
                   "rack.amount:" + cable.str() + ":" + std::to_string(gesture_));
    };
    sh.selectionChanged = [this](const std::set<model::Id>&) {
        if (status_) status_->setText(status());
    };
    sh.viewChanged = [this](const RackSpace::View& v) {
        if (rack_) views_[*rack_] = v;
        if (zoom_)
            zoom_->setText(std::to_string(static_cast<int>(std::lround(v.zoom * 100))) + " %");
        // What is in view changed: watch it from the next tick.
        nextTelemetry_ = std::chrono::steady_clock::now();
    };
    sh.deleteSelected = [this] { deleteSelected(); };
    sh.spaceTap = [this] {
        if (spaceTap) spaceTap();
    };
    space_ = &body.emplace<RackSpace>(std::move(sh));
    auto& bar = content->emplace<StatusBar>();
    status_ = &bar.emplace<ui::Label>("rack.status", "", ui::Font::Ui, "small", "ink-quiet");
    status_->layout.width = ui::SizeRule::fraction();
    panel_.open(std::move(content), [this] {
        watch(false);
        space_ = nullptr;
        tabs_ = nullptr;
        browser_ = nullptr;
        browserColumn_ = nullptr;
        status_ = nullptr;
        zoom_ = nullptr;
        browserToggle_ = nullptr;
        nextTelemetry_.reset();
    });
    version_ = projectGeneration_ = ~std::uint64_t{0};
    if (auto r = reload(); !r) report(r.error());
    root().focus(space_);
}

Status RackWindow::reload() {
    const model::Session& s = app_.head();
    catalog_ = racks::catalog(app_);
    projectGeneration_ = app_.projectGeneration();
    version_ = s.version;
    if (rack_ && !s.racks.find(*rack_)) rack_.reset();
    if (!rack_ && !s.racks.empty()) rack_ = s.racks.begin()->id;
    refreshTabs();
    if (browser_) {
        std::vector<RackBrowser::Entry> entries;
        for (const rack::Catalog::Entry* e : catalog_->list()) {
            const auto& info = e->type->info();
            entries.push_back({info.id, info.name, info.description, info.tags, e->type->origin()});
        }
        browser_->setEntries(std::move(entries));
    }
    if (!space_) return {};
    const bool other = !(rack_ ? space_->scene().rack == *rack_ : false);
    RackScene scene = rack_ ? RackScene::build(s, *rack_, *catalog_) : RackScene{};
    space_->setScene(std::move(scene));
    space_->setEmptyText(s.racks.empty() ? "No racks yet. + adds one on the selected channel."
                                         : "");
    if (other && rack_) {
        if (const auto v = views_.find(*rack_); v != views_.end())
            space_->setView(v->second);
        else
            space_->frameAllWhenLaidOut();
    }
    nextTelemetry_ = std::chrono::steady_clock::now();
    if (status_) status_->setText(status());
    return {};
}

void RackWindow::refreshTabs() {
    if (!tabs_) return;
    const model::Session& s = app_.head();
    std::vector<RackTabs::Tab> tabs;
    for (const auto& e : s.racks) {
        RackTabs::Tab t;
        t.rack = e.id;
        t.label = model::rackLabel(s, e.id);
        if (const model::Track* tr = s.tracks.find(e.value->track))
            t.color = root().theme().trackColor(tr->color);
        tabs.push_back(std::move(t));
    }
    tabs_->set(std::move(tabs), rack_);
    panel_.setTitle(rack_ ? "Racks — " + model::rackLabel(s, *rack_) : std::string("Racks"));
}

void RackWindow::show(std::optional<model::Id> rack) {
    if (rack && !app_.head().racks.find(*rack)) return;
    if (rack == rack_ && space_ && space_->scene().rack == rack_.value_or(model::Id{})) return;
    watch(false);
    rack_ = rack;
    error_.clear();
    version_ = ~std::uint64_t{0};
    (void)reload();
}

void RackWindow::sync() {
    if (!isOpen()) return;
    if (app_.projectGeneration() != projectGeneration_ || app_.head().version != version_) {
        error_.clear();
        (void)reload();
    }
}

void RackWindow::watch(bool on) {
    if (on) return;
    for (model::Id id : watched_)
        if (rack::Instance* in = racks::instance(app_, id)) in->watch(false);
    watched_.clear();
}

bool RackWindow::tick(std::chrono::steady_clock::time_point now) {
    if (!isOpen() || !nextTelemetry_ || now < *nextTelemetry_) return false;
    nextTelemetry_ = now + kTelemetryInterval;
    bool changed = false;
    const ui::Rect seen = space_->visibleSpace();
    std::vector<model::Id> watching;
    for (auto& m : space_->scene().modules) {
        rack::Instance* in = racks::instance(app_, m.id);
        const bool visible = !seen.intersect(m.rect()).empty();
        if (!in || !visible || !m.type) {
            if (m.live) {
                m.live = false;
                changed = true;
            }
            continue;
        }
        in->watch(true);
        watching.push_back(m.id);
        if (in->readTelemetry(m.telemetry)) {
            m.live = true;
            changed = true;
        }
    }
    for (model::Id id : watched_)
        if (std::find(watching.begin(), watching.end(), id) == watching.end())
            if (rack::Instance* in = racks::instance(app_, id)) in->watch(false);
    watched_ = std::move(watching);
    if (watched_.empty()) nextTelemetry_ = now + std::chrono::milliseconds(250);
    if (changed) space_->telemetryChanged();
    return changed;
}

std::optional<std::chrono::steady_clock::time_point> RackWindow::nextDeadline() const {
    return isOpen() ? nextTelemetry_ : std::nullopt;
}

json::Value RackWindow::saveLayout() const {
    json::Value out = json::Value::object();
    if (rack_) out["rack"] = rack_->str();
    out["browser"] = browserVisible_;
    json::Value views = json::Value::object();
    for (const auto& [id, v] : views_) {
        if (!app_.head().racks.find(id)) continue;
        json::Value one = json::Value::object();
        one["x"] = std::round(v.x * 10) / 10;
        one["y"] = std::round(v.y * 10) / 10;
        one["zoom"] = std::round(v.zoom * 1000) / 1000;
        views[id.str()] = std::move(one);
    }
    out["views"] = std::move(views);
    return out;
}

void RackWindow::loadLayout(const json::Value& layout) {
    views_.clear();
    if (!layout.isObject()) return;
    if (const auto* r = layout.find("rack"); r && r->isString())
        if (auto id = model::Id::parse(r->asString()); id && id->kind == model::Kind::Rack)
            rack_ = id;
    if (const auto* b = layout.find("browser"); b && b->isBool()) browserVisible_ = b->asBool();
    if (const auto* views = layout.find("views"); views && views->isObject())
        for (const auto& m : views->asObject()) {
            const auto id = model::Id::parse(m.key);
            if (!id || id->kind != model::Kind::Rack || !m.value.isObject()) continue;
            auto num = [&](const char* k, double def) {
                const auto* v = m.value.find(k);
                return v && v->isNumber() && std::isfinite(v->asDouble()) ? v->asDouble() : def;
            };
            views_[*id] = {num("x", 0), num("y", 0),
                           std::clamp(num("zoom", 1), RackSpace::kMinZoom, RackSpace::kMaxZoom)};
        }
    if (isOpen()) {
        version_ = ~std::uint64_t{0};
        if (browserColumn_) {
            browserColumn_->visible = browserVisible_;
            browserColumn_->relayout();
        }
        if (space_) space_->setScene({});  // so the saved view applies
        (void)reload();
    }
}

// --- edits ---------------------------------------------------------------------------

Status RackWindow::addRack(std::optional<model::Id> track) {
    const model::Session& s = app_.head();
    if (!track && !ui_.selectedTracks.empty())
        if (auto id = model::Id::parse(ui_.selectedTracks.front()))
            if (const model::Track* t = s.tracks.find(*id);
                t && t->type == model::StripType::Channel)
                track = id;
    json::Value args = json::Value::object();
    if (track) args["track"] = track->str();
    auto r = exec("rack.add", std::move(args));
    if (!r) return std::move(r).error();
    const auto id = model::Id::parse(str(*r, "rack"));
    open(id);
    show(id);
    return {};
}

Status RackWindow::addModule(const std::string& type, std::optional<ui::Point> at) {
    if (!rack_) {
        if (auto r = addRack(); !r) return r;
    }
    json::Value args = json::Value::object();
    args["rack"] = rack_->str();
    args["type"] = type;
    if (!at && space_) {  // the middle of the view
        const ui::Rect v = space_->visibleSpace();
        at = ui::Point{v.x + v.w / 2 - 50, v.y + v.h / 2 - 60};
    }
    if (at) {
        args["x"] = std::round(static_cast<double>(at->x));
        args["y"] = std::round(static_cast<double>(at->y));
    }
    auto r = exec("module.add", std::move(args));
    if (!r) return std::move(r).error();
    sync();
    if (space_)
        if (auto id = model::Id::parse(str(*r, "module"))) space_->select({*id});
    return {};
}

void RackWindow::move(const std::vector<RackSpace::Move>& moves) {
    auto r = dispatcher_.transact(
        moves.size() == 1 ? "Move module" : "Move modules", kOrigin, [&]() -> Result<json::Value> {
            for (const auto& m : moves) {
                json::Value args = json::Value::object();
                args["module"] = m.module.str();
                args["x"] = m.x;
                args["y"] = m.y;
                SUPPLE_TRY(dispatcher_.execute({"module.set", std::move(args), kOrigin}));
            }
            return json::Value::object();
        });
    if (!r) {
        report(r.error());
        version_ = ~std::uint64_t{0};  // back to where the session has them
        sync();
    }
}

void RackWindow::param(model::Id module, int index, double value, ui::Gesture g) {
    if (g == ui::Gesture::Begin) {
        ++gesture_;
        return;
    }
    if (g == ui::Gesture::End) return;
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m || !m->type || index < 0 || static_cast<std::size_t>(index) >= m->type->params().size())
        return;
    const auto& p = m->type->params()[static_cast<std::size_t>(index)];
    json::Value params = json::Value::object();
    params[p.key] = racks::paramJson(p, value);
    json::Value args = json::Value::object();
    args["module"] = module.str();
    args["params"] = std::move(params);
    (void)exec("module.set", std::move(args),
               g == ui::Gesture::Move
                   ? "rack.param:" + module.str() + ":" + p.key + ":" + std::to_string(gesture_)
                   : std::string());
}

void RackWindow::connect(const model::CableEnd& from, const model::CableEnd& to,
                         std::optional<model::Id> replace) {
    if (replace)
        if (const model::Cable* old = app_.head().cables.find(*replace);
            old && old->from == from && old->to == to)
            return;  // dropped back where it was
    auto add = [&]() -> Result<json::Value> {
        json::Value args = json::Value::object();
        args["from"] = from.str();
        args["to"] = to.str();
        return dispatcher_.execute({"cable.add", std::move(args), kOrigin});
    };
    Result<json::Value> r = json::Value();
    if (replace)
        r = dispatcher_.transact("Move cable", kOrigin, [&]() -> Result<json::Value> {
            json::Value args = json::Value::object();
            args["cable"] = replace->str();
            SUPPLE_TRY(dispatcher_.execute({"cable.remove", std::move(args), kOrigin}));
            return add();
        });
    else
        r = add();
    if (!r) report(r.error());
}

void RackWindow::deleteSelected() {
    if (!space_ || space_->selection().empty()) return;
    const auto ids = space_->selection();
    auto r = dispatcher_.transact(
        ids.size() == 1 ? "Remove module" : "Remove modules", kOrigin,
        [&]() -> Result<json::Value> {
            for (model::Id id : ids) {
                json::Value args = json::Value::object();
                args["module"] = id.str();
                SUPPLE_TRY(dispatcher_.execute({"module.remove", std::move(args), kOrigin}));
            }
            return json::Value::object();
        });
    if (!r) report(r.error());
}

void RackWindow::moduleMenu(model::Id module, ui::Point at) {
    const model::RackModule* m = app_.head().modules.find(module);
    if (!m || !space_) return;
    const auto selected = space_->selection();
    const bool several = selected.size() > 1;
    std::vector<ui::MenuItem> items;
    auto setOn = [this](model::Id id, const char* field, json::Value v) {
        json::Value args = json::Value::object();
        args["module"] = id.str();
        args[field] = std::move(v);
        (void)exec("module.set", std::move(args));
    };
    if (m->fx && openEditor)
        items.push_back({"Open editor", "", [this, fx = *m->fx] {
                             if (auto r = openEditor(fx); !r) report(r.error());
                         }});
    items.push_back({"Rename…", "", [this, module] { rename(module); }, !several});
    items.push_back(
        {"Bypass", "",
         [this, module, setOn, bypass = m->bypass, selected] {
             auto r = dispatcher_.transact("Bypass", kOrigin, [&]() -> Result<json::Value> {
                 for (model::Id id : selected) {
                     json::Value args = json::Value::object();
                     args["module"] = id.str();
                     args["bypass"] = !bypass;
                     SUPPLE_TRY(dispatcher_.execute({"module.set", std::move(args), kOrigin}));
                 }
                 return json::Value::object();
             });
             if (!r) report(r.error());
             (void)module;
             (void)setOn;
         },
         true, false, m->bypass});
    items.push_back({"Duplicate", "", [this, selected] {
                         json::Value args = json::Value::object();
                         json::Value list = json::Value::array();
                         for (model::Id id : selected) list.push_back(id.str());
                         args["modules"] = std::move(list);
                         if (auto r = exec("module.duplicate", std::move(args)); r && space_) {
                             std::set<model::Id> made;
                             if (const auto* ids = r->find("modules"); ids && ids->isArray())
                                 for (const auto& x : ids->asArray())
                                     if (auto id = x.isString() ? model::Id::parse(x.asString())
                                                                : std::nullopt)
                                         made.insert(*id);
                             sync();
                             space_->select(std::move(made));
                         }
                     }});
    const auto* type = catalog_ ? catalog_->find(m->type) : nullptr;
    const bool hasKnobs = type && !type->type->params().empty();
    items.push_back({"Randomize", "",
                     [this, module] {
                         json::Value args = json::Value::object();
                         args["module"] = module.str();
                         (void)exec("module.randomize", std::move(args));
                     },
                     hasKnobs && !several});
    // Presets: the type's factory patches.
    std::vector<std::string> presets;
    if (type && type->type->code())
        for (const auto& patch : type->type->code()->factoryPatches())
            presets.push_back(patch.name);
    if (!presets.empty())
        items.push_back({"Presets…", "",
                         [this, module, presets, at] {
                             std::vector<ui::MenuItem> list;
                             for (const auto& name : presets)
                                 list.push_back({name, "", [this, module, name] {
                                                     json::Value args = json::Value::object();
                                                     args["module"] = module.str();
                                                     args["preset"] = name;
                                                     (void)exec("module.set", std::move(args));
                                                 }});
                             root().openPopup(ui::makeMenu("rack.presets.menu", std::move(list),
                                                           [this] { root().closePopup(); }),
                                              {at.x, at.y, 1, 1});
                         },
                         !several});
    if (type && !type->type->source().empty())
        items.push_back({"Show source", "",
                         [this, source = type->type->source(), at] {
                             auto text = std::make_unique<ui::Node>("rack.source");
                             (void)source;
                             (void)at;
                             (void)text;
                             report(Error("gui.rack", "the source view is not built yet"));
                         },
                         !several});
    items.push_back({"", "", {}, true, true});
    items.push_back({several ? "Delete modules" : "Delete", "Del", [this] { deleteSelected(); }});
    root().openPopup(
        ui::makeMenu("rack.module.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::spaceMenu(ui::Point at, ui::Point sp) {
    if (!catalog_) return;
    std::vector<ui::MenuItem> items;
    if (!rack_) {
        items.push_back({"Add rack", "", [this] { (void)addRack(); }});
    } else {
        for (const rack::Catalog::Entry* e : catalog_->list()) {
            const auto& info = e->type->info();
            items.push_back(
                {"Add " + info.name, "", [this, id = info.id, sp] { (void)addModule(id, sp); }});
        }
        items.push_back({"", "", {}, true, true});
        items.push_back({"Show everything", "Home", [this] {
                             if (space_) space_->frameAll();
                         }});
    }
    root().openPopup(
        ui::makeMenu("rack.space.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::paramMenu(model::Id module, int index, ui::Point at) {
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m || !m->type || index < 0 || static_cast<std::size_t>(index) >= m->type->params().size())
        return;
    const auto& p = m->type->params()[static_cast<std::size_t>(index)];
    std::vector<ui::MenuItem> items;
    items.push_back(
        {"Default (" + paramText(p, p.def) + ")", "Double-click",
         [this, module, index, def = p.def] { param(module, index, def, ui::Gesture::Set); }});
    if (p.kind == script::detail::ParamModel::Kind::Choice) {
        items.push_back({"", "", {}, true, true});
        const double v = static_cast<std::size_t>(index) < m->values.size()
                             ? m->values[static_cast<std::size_t>(index)]
                             : p.def;
        for (std::size_t k = 0; k < p.choices.size(); ++k)
            items.push_back({p.choices[k], "",
                             [this, module, index, k] {
                                 param(module, index, static_cast<double>(k), ui::Gesture::Set);
                             },
                             true, false, std::lround(v) == static_cast<long>(k)});
    }
    root().openPopup(
        ui::makeMenu("rack.param.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::tabMenu(model::Id rack, ui::Point at) {
    const model::Session& s = app_.head();
    std::vector<ui::MenuItem> items;
    items.push_back({"Rename…", "", [this, rack] { renameRack(rack); }});
    items.push_back({"", "", {}, true, true});
    const model::Rack* r = s.racks.find(rack);
    for (const auto& e : s.tracks) {
        if (e.value->type != model::StripType::Channel) continue;
        const bool home = r && r->track == e.id;
        items.push_back({"Home: " + model::trackLabel(s, e.id), "",
                         [this, rack, track = e.id] {
                             json::Value args = json::Value::object();
                             args["rack"] = rack.str();
                             args["track"] = track.str();
                             (void)exec("rack.set", std::move(args));
                         },
                         !home, false, home, root().theme().trackColor(e.value->color)});
    }
    items.push_back({"", "", {}, true, true});
    items.push_back({"Remove rack", "", [this, rack] {
                         json::Value args = json::Value::object();
                         args["rack"] = rack.str();
                         (void)exec("rack.remove", std::move(args));
                     }});
    root().openPopup(
        ui::makeMenu("rack.tab.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

namespace {

// A name field over `where`: Enter submits, Escape and a click away cancel.
void openNameField(ui::Root& root, std::string id, std::string text, ui::Rect where,
                   std::function<void(const std::string&)> submit) {
    auto field = std::make_unique<ui::TextField>(std::move(id), std::move(text));
    ui::TextField* edit = field.get();
    edit->layout.width = ui::SizeRule::fixed(std::max(where.w, 160.0f));
    edit->layout.height = ui::SizeRule::fixed(24);
    edit->onSubmit = [&root, submit = std::move(submit)](const std::string& t) {
        const std::string copy = t;  // closing destroys the field
        root.closePopup();
        submit(copy);
    };
    edit->onCancel = [&root] { root.closePopup(); };
    root.openPopup(std::move(field), {where.x, where.y, where.w, 0});
    edit->selectAll();
    root.focus(edit);
}

}  // namespace

void RackWindow::rename(model::Id module) {
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m) return;
    const ui::Point a = space_->toWindow({static_cast<float>(m->x), static_cast<float>(m->y)});
    const float w = static_cast<float>(m->w * space_->view().zoom);
    const model::RackModule* mm = app_.head().modules.find(module);
    openNameField(root(), "rack.rename", mm && !mm->label.empty() ? mm->label : m->title,
                  {a.x, a.y, w, 0}, [this, module](const std::string& text) {
                      json::Value args = json::Value::object();
                      args["module"] = module.str();
                      args["label"] = text;
                      (void)exec("module.set", std::move(args));
                  });
}

void RackWindow::renameRack(model::Id rack) {
    if (!tabs_) return;
    const auto r = tabs_->tabRect(rack);
    const model::Rack* rk = app_.head().racks.find(rack);
    if (!r || !rk) return;
    openNameField(root(), "rack.tab.rename", model::rackLabel(app_.head(), rack),
                  {r->x, r->y, r->w, 0}, [this, rack](const std::string& text) {
                      json::Value args = json::Value::object();
                      args["rack"] = rack.str();
                      args["name"] = text;
                      (void)exec("rack.set", std::move(args));
                  });
}

}  // namespace supple::gui
