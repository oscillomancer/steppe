// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "routing_matrix.h"

#include <SDL3/SDL_keycode.h>
#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "strip_learn.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

// The send controls' row: a right-click there is the row's (`onContext`
// says whether it used it); the controls take only the left button.
class ControlRow : public ui::Node {
  public:
    explicit ControlRow(std::string id) : Node(std::move(id)) {}
    std::function<bool(ui::Point)> onContext;
    bool onPointer(const ui::PointerEvent& e) override {
        return e.type == ui::PointerEvent::Type::Down && e.button == 3 && onContext &&
               onContext(e.pos);
    }
};
constexpr float kLabelW = 190, kHeaderH = 112, kCell = 22;

std::string fmt(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

}  // namespace

void MatrixModel::index() {
    cycleIndex.clear();
    sendIndex.clear();
    for (std::size_t i = 0; i < cyclePairs.size(); ++i) cycleIndex[cyclePairs[i]] = cyclePaths[i];
    for (std::size_t i = 0; i < sends.size(); ++i)
        sendIndex[{sends[i].from, sends[i].to}].push_back(i);
}

int MatrixModel::sourcePair(const Send& x) {
    if (x.mapFrom.empty()) return 1;
    const int first = *std::min_element(x.mapFrom.begin(), x.mapFrom.end());
    return first <= 2 ? 1 : first % 2 == 1 ? first : first - 1;
}

std::string MatrixModel::pairText(int pair, int width) {
    return width < 2 ? std::to_string(pair) : std::to_string(pair) + "/" + std::to_string(pair + 1);
}

std::string MatrixModel::blocked(std::size_t row, std::size_t col) const {
    const Source& s = sources[row];
    const Destination& d = destinations[col];
    if (s.pair != 1 && d.kind == "key")
        return "a key takes a strip's main pair; the plug-in's pins can read others";
    if (d.kind == "track") {
        if (s.id == d.id) return "a strip cannot send to itself";
        if (!s.rule.empty()) return s.rule;
        if (!d.rule.empty()) return d.rule;
    } else if (d.kind == "key") {
        if (s.id == d.track) return "a strip cannot key a plug-in in its own chain";
    } else if (!s.rule.empty()) {  // an In module: masters are terminal
        return s.rule;
    }
    if (const auto found = cycleIndex.find({s.id, d.id}); found != cycleIndex.end())
        return "it would feed back: " + found->second;
    return {};
}

std::vector<const MatrixModel::Send*> MatrixModel::sendsAt(std::size_t row, std::size_t col) const {
    std::vector<const Send*> out;
    if (const auto found = sendIndex.find({sources[row].id, destinations[col].id});
        found != sendIndex.end())
        for (auto index : found->second)
            if (sourcePair(sends[index]) == sources[row].pair) out.push_back(&sends[index]);
    return out;
}

namespace {

// What the grid's clicks do (RoutingMatrix carries them out).
struct GridHooks {
    std::function<void(std::size_t, std::size_t, unsigned)> cell;
    std::function<void(std::size_t, std::size_t)> doubleCell;
    std::function<void(std::size_t, unsigned)> row;
    std::function<void(std::size_t, unsigned)> column;
    std::function<void()> all;
    std::function<void()> remove;
    std::function<bool(std::size_t, std::size_t)> selected;
    std::function<void(std::optional<std::pair<std::size_t, std::size_t>>)> hover;
};

class Grid : public ui::Node {
  public:
    Grid(const MatrixModel& m, GridHooks hooks)
        : Node("routing.grid"), m_(m), hooks_(std::move(hooks)) {
        interactive = true;
        focusable = true;  // Ctrl+A, Delete
        layout.height = ui::SizeRule::fraction();
    }

    void modelChanged() {
        scrollX_ = std::clamp(scrollX_, 0.0f,
                              std::max(0.0f, static_cast<float>(m_.destinations.size()) * kCell -
                                                 std::max(0.0f, bounds().w - kLabelW)));
        scrollY_ = std::clamp(scrollY_, 0.0f,
                              std::max(0.0f, static_cast<float>(m_.sources.size()) * kCell -
                                                 std::max(0.0f, bounds().h - kHeaderH)));
        hovered_.reset();
        invalidate();
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& bounds) override {
        Node::arrange(canvas, bounds);
        modelChanged();
    }

    std::optional<std::pair<std::size_t, std::size_t>> cellAt(ui::Point p) const {
        const auto r = rowAt(p.y);
        const auto c = columnAt(p.x);
        if (!r || !c || p.x < bounds().x + kLabelW || p.y < bounds().y + kHeaderH)
            return std::nullopt;
        return std::pair{*r, *c};
    }
    std::optional<std::size_t> rowAt(float py) const {
        const float y = py - (bounds().y + kHeaderH) + scrollY_;
        if (py < bounds().y + kHeaderH || y < 0) return std::nullopt;
        const auto row = static_cast<std::size_t>(y / kCell);
        return row < m_.sources.size() ? std::optional(row) : std::nullopt;
    }
    std::optional<std::size_t> columnAt(float px) const {
        const float x = px - (bounds().x + kLabelW) + scrollX_;
        if (px < bounds().x + kLabelW || x < 0) return std::nullopt;
        const auto col = static_cast<std::size_t>(x / kCell);
        return col < m_.destinations.size() ? std::optional(col) : std::nullopt;
    }

    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRect(b, t.color("panel"));
        const float gx = b.x + kLabelW - scrollX_, gy = b.y + kHeaderH - scrollY_;
        const ui::Rect cells{b.x + kLabelW, b.y + kHeaderH, b.w - kLabelW, b.h - kHeaderH};
        auto typeColor = [&](const std::string& type) {
            return t.color(type == "master" ? "master" : type == "buss" ? "buss" : "channel");
        };
        // Column headers: rotated names.
        c.save();
        c.clip({cells.x, b.y, cells.w, kHeaderH});
        NVGcontext* vg = c.vg();
        for (std::size_t col = 0; col < m_.destinations.size(); ++col) {
            const auto& d = m_.destinations[col];
            const float x = gx + static_cast<float>(col) * kCell;
            if (x + kCell < cells.x || x > cells.right()) continue;
            // A master's mark is heavier (its spine, gui.md §7).
            const float mark = d.type == "master" ? 5.0f : 3.0f;
            // A key's mark is violet, a rack In module's teal: hues no strip
            // type uses.
            c.fillRect({x + 4, b.y + kHeaderH - 3 - mark, kCell - 8, mark},
                       d.kind == "key"      ? t.color("agent")
                       : d.kind == "module" ? t.color("track-teal")
                                            : typeColor(d.type));
            nvgSave(vg);
            nvgTranslate(vg, x + kCell / 2 + 4, b.y + kHeaderH - 10);
            nvgRotate(vg, -static_cast<float>(M_PI) / 3);
            c.text({0, -8, kHeaderH * 1.05f, 16}, d.name.empty() ? d.id : d.name, ui::Font::Ui,
                   t.type("caption"), d.kind == "key" ? t.color("ink-quiet") : t.color("ink"),
                   ui::kLeft | ui::kMiddle);
            nvgRestore(vg);
        }
        c.restore();
        // Row labels: indented by depth, type colour bar.
        c.save();
        c.clip({b.x, cells.y, kLabelW, cells.h});
        for (std::size_t row = 0; row < m_.sources.size(); ++row) {
            const auto& s = m_.sources[row];
            const float y = gy + static_cast<float>(row) * kCell;
            if (y + kCell < cells.y || y > cells.bottom()) continue;
            const float indent = 8 + static_cast<float>(s.depth) * 12;
            if (s.pair != 1) {  // an auxiliary pair, under its strip: quieter, no bar
                c.text({b.x + indent, y, kLabelW - indent - 6, kCell}, s.name, ui::Font::Ui,
                       t.type("caption"), t.color("ink-quiet"));
                continue;
            }
            const float mark = s.type == "master" ? 5.0f : 3.0f;
            c.fillRect({b.x + indent - 3 - mark, y + 4, mark, kCell - 8}, typeColor(s.type));
            c.text({b.x + indent, y, kLabelW - indent - 6, kCell}, s.name.empty() ? s.id : s.name,
                   ui::Font::Ui, t.type("small"), t.color("ink"));
        }
        c.restore();
        // Cells.
        c.save();
        c.clip(cells);
        for (std::size_t row = 0; row < m_.sources.size(); ++row) {
            const float y = gy + static_cast<float>(row) * kCell;
            if (y + kCell < cells.y || y > cells.bottom()) continue;
            for (std::size_t col = 0; col < m_.destinations.size(); ++col) {
                const float x = gx + static_cast<float>(col) * kCell;
                if (x + kCell < cells.x || x > cells.right()) continue;
                const ui::Rect r{x + 1, y + 1, kCell - 2, kCell - 2};
                const auto sends = m_.sendsAt(row, col);
                const bool blocked = sends.empty() && !m_.blocked(row, col).empty();
                const bool hovered = hovered_ && hovered_->first == row && hovered_->second == col;
                const bool sel = hooks_.selected(row, col);
                if (blocked) {
                    c.fillRect(r, t.color("ground").withAlpha(0.7f));
                    c.line({r.x + 4, r.bottom() - 4}, {r.right() - 4, r.y + 4}, t.color("rule"), 1);
                    if (sel) c.fillRect(r, t.color("selection").withAlpha(0.5f));
                } else {
                    c.fillRect(r, sel       ? t.color("selection")
                                  : hovered ? t.color("selection").withAlpha(0.5f)
                                            : t.color("raised"));
                }
                if (!sends.empty()) {
                    const ui::Rect dot{r.x + 4, r.y + 4, r.w - 8, r.h - 8};
                    c.fillRound(dot, 3,
                                sends.front()->mute ? t.color("ink-faint") : t.color("accent"));
                }
                if (sel)
                    c.strokeRound({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 3, t.color("focus"), 2);
                // The implicit parent send: outlined when on, a faint corner mark when off.
                if (m_.destinations[col].kind == "track" && m_.sources[row].pair == 1 &&
                    m_.sources[row].parent == m_.destinations[col].id) {
                    if (m_.sources[row].parentSend)
                        c.strokeRect({r.x + 2, r.y + 2, r.w - 4, r.h - 4}, t.color("ink-quiet"));
                    else
                        c.fillRect({r.x + 2, r.y + 2, 4, 4}, t.color("ink-faint"));
                }
            }
        }
        c.restore();
        c.fillRect({cells.x - 1, b.y, 1, b.h}, t.color("rule"));
        c.fillRect({b.x, cells.y - 1, b.w, 1}, t.color("rule"));
    }

    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        if (e.type == T::Move || e.type == T::Enter) {
            const auto cell = cellAt(e.pos);
            if (cell != hovered_) {
                hovered_ = cell;
                hooks_.hover(cell);
                invalidate();
            }
            return true;
        }
        if (e.type == T::Leave) {
            hovered_.reset();
            hooks_.hover(std::nullopt);
            invalidate();
            return true;
        }
        if (e.type == T::Down && e.button == 1) {
            const ui::Rect b = bounds();
            const bool inLabels = e.pos.x < b.x + kLabelW, inHeader = e.pos.y < b.y + kHeaderH;
            if (inLabels && inHeader)
                hooks_.all();
            else if (inLabels) {
                if (auto row = rowAt(e.pos.y)) hooks_.row(*row, e.mods);
            } else if (inHeader) {
                if (auto col = columnAt(e.pos.x)) hooks_.column(*col, e.mods);
            } else if (auto cell = cellAt(e.pos)) {
                if (e.clicks >= 2)
                    hooks_.doubleCell(cell->first, cell->second);
                else
                    hooks_.cell(cell->first, cell->second, e.mods);
            }
            invalidate();
            return true;
        }
        if (e.type == T::Wheel) {
            const float maxX = std::max(
                0.0f, static_cast<float>(m_.destinations.size()) * kCell - (bounds().w - kLabelW));
            const float maxY = std::max(
                0.0f, static_cast<float>(m_.sources.size()) * kCell - (bounds().h - kHeaderH));
            if (e.mods & ui::kShift)
                scrollX_ = std::clamp(scrollX_ - (e.wheelY + e.wheelX) * kCell, 0.0f, maxX);
            else {
                scrollY_ = std::clamp(scrollY_ - e.wheelY * kCell * 2, 0.0f, maxY);
                scrollX_ = std::clamp(scrollX_ - e.wheelX * kCell, 0.0f, maxX);
            }
            invalidate();
            return true;
        }
        return false;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_A && (e.mods & ui::kCtrl)) {
            hooks_.all();
            invalidate();
            return true;
        }
        if ((e.key == SDLK_DELETE || e.key == SDLK_BACKSPACE) && !e.mods) {
            hooks_.remove();
            return true;
        }
        return false;
    }
    // A cell's centre in window coordinates (tests).
    ui::Point centre(std::size_t row, std::size_t col) const {
        const ui::Rect b = bounds();
        return {b.x + kLabelW - scrollX_ + (static_cast<float>(col) + 0.5f) * kCell,
                b.y + kHeaderH - scrollY_ + (static_cast<float>(row) + 0.5f) * kCell};
    }

  private:
    const MatrixModel& m_;
    GridHooks hooks_;
    std::optional<std::pair<std::size_t, std::size_t>> hovered_;
    float scrollX_ = 0, scrollY_ = 0;
};

std::string panText(double pan) {
    const long p = std::lround(pan);
    return p == 0 ? std::string("C") : (p < 0 ? "L " : "R ") + std::to_string(std::labs(p));
}

std::string dbText(double db) {
    return db <= ui::kFaderSilenceDb ? std::string("-inf dB") : fmt(db, 1) + " dB";
}

}  // namespace

RoutingMatrix::RoutingMatrix(FloatingPanel& panel, AppActions& actions, cmd::Dispatcher& dispatcher,
                             engine::EngineApp& app)
    : panel_(panel), actions_(actions), dispatcher_(dispatcher), app_(app) {
    actions_.addUi("ui.routing", "View: routing matrix", [this] { open(); });
}

RoutingMatrix::~RoutingMatrix() { close(); }

Result<json::Value> RoutingMatrix::exec(const std::string& command, json::Value args,
                                        const cmd::Origin* origin) {
    auto r = dispatcher_.execute({command, std::move(args), origin ? *origin : kOrigin});
    if (!r && status_) status_->setText(command + ": " + r.error().message);
    return r;
}

Status RoutingMatrix::reload() {
    auto m = dispatcher_.execute({"route.matrix", json::Value::object(), kOrigin});
    if (!m) return std::move(m).error();
    auto list = dispatcher_.execute({"route.list", json::Value::object(), kOrigin});
    if (!list) return std::move(list).error();
    MatrixModel next;
    auto str = [](const json::Value& v, const char* k) {
        const json::Value* x = v.find(k);
        return x && x->isString() ? x->asString() : std::string();
    };
    for (const auto& s : m->find("sources")->asArray()) {
        MatrixModel::Source src;
        src.id = str(s, "id");
        src.name = str(s, "name");
        src.type = str(s, "type");
        src.parent = str(s, "parent");
        src.depth = static_cast<int>(s.find("depth")->asInt());
        if (const auto* ps = s.find("parent_send")) src.parentSend = ps->asBool();
        if (const auto* rule = s.find("rule")) src.rule = str(*rule, "message");
        const int channels = s.find("channels") ? static_cast<int>(s.find("channels")->asInt()) : 2;
        const json::Value* names = s.find("channel_names");
        // A row per auxiliary pair under the strip's own (ADR-0021).
        std::vector<MatrixModel::Source> pairs;
        for (int c = 3; c <= channels; c += 2) {
            MatrixModel::Source aux = src;
            aux.pair = c;
            aux.width = c + 1 <= channels ? 2 : 1;
            aux.strip = src.name.empty() ? src.id : src.name;
            aux.name = MatrixModel::pairText(c, aux.width);
            if (const json::Value* n = names ? names->find(std::to_string(c)) : nullptr;
                n && n->isString())
                aux.name += "  " + n->asString();
            aux.depth = src.depth + 1;
            pairs.push_back(std::move(aux));
        }
        next.sources.push_back(std::move(src));
        for (auto& aux : pairs) next.sources.push_back(std::move(aux));
    }
    for (const auto& d : m->find("destinations")->asArray()) {
        MatrixModel::Destination dst;
        dst.id = str(d, "id");
        dst.name = str(d, "name");
        dst.kind = str(d, "kind");
        dst.type = str(d, "type");
        dst.track = str(d, "track");
        if (const auto* rule = d.find("rule")) dst.rule = str(*rule, "message");
        next.destinations.push_back(std::move(dst));
    }
    for (const auto& c : m->find("cycles")->asArray()) {
        next.cyclePairs.emplace_back(str(c, "from"), str(c, "to"));
        next.cyclePaths.push_back(str(c, "path"));
    }
    for (const auto& r : list->find("routes")->asArray()) {
        // Only sends from a strip are the matrix's. A channel's hardware or
        // MIDI input has no source strip, and a rack's Out (a channel's input)
        // and Note Out are not strip sends.
        const std::string from = str(r, "from");
        if (from.empty() || from.starts_with("mo_") || str(r, "to").empty()) continue;
        MatrixModel::Send x;
        x.id = str(r, "id");
        x.from = str(r, "from");
        x.to = str(r, "fx").empty() ? str(r, "to") : str(r, "fx");
        x.tap = str(r, "tap");
        if (const auto* v = r.find("volume_db"); v && v->isNumber()) x.volumeDb = v->asDouble();
        if (const auto* v = r.find("pan"); v && v->isNumber()) x.pan = v->asDouble();
        if (const auto* v = r.find("mute"); v && v->isBool()) x.mute = v->asBool();
        if (const auto* v = r.find("mono"); v && v->isBool()) x.mono = v->asBool();
        if (const auto* v = r.find("phase_invert"); v && v->isBool()) x.phaseInvert = v->asBool();
        if (const auto* ch = r.find("channels"); ch && ch->isObject()) {
            for (const char* side : {"from", "to"})
                if (const auto* ids = ch->find(side); ids && ids->isArray())
                    for (const auto& n : ids->asArray())
                        if (n.isNumber())
                            (side[0] == 'f' ? x.mapFrom : x.mapTo)
                                .push_back(static_cast<int>(n.asDouble()));
        }
        next.sends.push_back(std::move(x));
    }
    next.index();
    model_ = std::move(next);
    if (grid_) static_cast<Grid*>(grid_)->modelChanged();
    projectGeneration_ = app_.projectGeneration();
    version_ = app_.head().version;
    // Cells whose strips went go from the selection.
    std::erase_if(selection_, [&](const Cell& c) { return !locate(c); });
    if (anchor_ && !locate(*anchor_)) anchor_.reset();
    refresh();
    return {};
}

void RoutingMatrix::open(std::optional<ui::Rect> anchor) {
    if (isOpen()) {
        panel_.raise();
        return;
    }
    if (auto ready = panel_.ensure(); !ready) {
        if (actions_.onError) actions_.onError(ready.error().message);
        return;
    }
    auto panel = std::make_unique<ui::Node>("routing");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fraction();
    panel->layout.height = ui::SizeRule::fraction();
    panel->layout.padding = ui::Insets::all(10);
    panel->layout.gap = 8;
    auto& header = panel->emplace<ui::Node>("routing.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(24);
    header.layout.align = ui::Layout::Align::Center;
    auto& title = header.emplace<ui::Label>("routing.title", "Routing", ui::Font::UiBold, "body");
    title.layout.width = ui::SizeRule::fraction();
    panel_.setTitle(title.text());
    header.emplace<ui::Button>("routing.close", "Close", [this] { close(); });
    GridHooks hooks;
    hooks.cell = [this](std::size_t row, std::size_t col, unsigned mods) {
        (void)clickCell(row, col, mods);
    };
    hooks.doubleCell = [this](std::size_t row, std::size_t col) {
        (void)doubleClickCell(row, col);
    };
    hooks.row = [this](std::size_t row, unsigned mods) { clickRow(row, mods); };
    hooks.column = [this](std::size_t col, unsigned mods) { clickColumn(col, mods); };
    hooks.all = [this] { selectAll(); };
    hooks.remove = [this] { (void)removeSelected(); };
    hooks.selected = [this](std::size_t row, std::size_t col) { return isSelected(row, col); };
    hooks.hover = [this](std::optional<std::pair<std::size_t, std::size_t>> cell) {
        if (!status_) return;
        if (!cell) {
            status_->setText(status());
            return;
        }
        const auto& s = model_.sources[cell->first];
        const auto& d = model_.destinations[cell->second];
        const std::string from =
            s.pair != 1 ? s.strip + " " + s.name : (s.name.empty() ? s.id : s.name);
        const std::string pair = from + " → " + (d.name.empty() ? d.id : d.name);
        const auto sends = model_.sendsAt(cell->first, cell->second);
        if (!sends.empty())
            status_->setText(pair + ": " + dbText(sends.front()->volumeDb) + ", " +
                             sends.front()->tap + (sends.front()->mute ? ", muted" : "") +
                             " — click to select");
        else if (const std::string why = model_.blocked(cell->first, cell->second); !why.empty())
            status_->setText(pair + ": not allowed — " + why);
        else if (parentCell(cell->first, cell->second))
            status_->setText(pair + (s.parentSend
                                         ? ": the parent send, on — double-click to turn it off"
                                         : ": the parent send, off — double-click to turn it on"));
        else
            status_->setText(pair + ": double-click to send");
    };
    auto& grid = panel->emplace<Grid>(model_, std::move(hooks));
    grid_ = &grid;
    details_ = &panel->emplace<ui::Node>("routing.details");
    details_->layout.dir = ui::Layout::Dir::Column;
    details_->layout.height = ui::SizeRule::fixed(62);
    details_->layout.gap = 6;
    // First row: what is selected, and Add / Remove.
    auto& top = details_->emplace<ui::Node>("routing.details.top");
    top.layout.dir = ui::Layout::Dir::Row;
    top.layout.height = ui::SizeRule::fixed(28);
    top.layout.gap = 6;
    top.layout.align = ui::Layout::Align::Center;
    sendTitle_ = &top.emplace<ui::Label>("routing.send", "", ui::Font::UiBold, "small");
    sendTitle_->layout.width = ui::SizeRule::fraction();
    add_ = &top.emplace<ui::Button>("routing.add", "Add", [this] { (void)addSelected(); });
    remove_ =
        &top.emplace<ui::Button>("routing.remove", "Remove", [this] { (void)removeSelected(); });
    // Second row: the selected sends' controls. A right-click on one learns a
    // MIDI control for it on every selected send (ADR-0058).
    auto& row = details_->emplace<ControlRow>("routing.details.controls");
    row.onContext = [this](ui::Point p) { return learnAt(p); };
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(28);
    row.layout.gap = 6;
    row.layout.align = ui::Layout::Align::Center;
    for (auto [id, label, tap] : {std::tuple{"routing.tap.post", "Post", "post_fader"},
                                  {"routing.tap.pre", "Pre", "pre_fader"},
                                  {"routing.tap.prefx", "Pre-FX", "pre_fx"}}) {
        const std::string value = tap;
        taps_.push_back(&row.emplace<ui::Button>(id, label, [this, value] {
            (void)setSends("Send tap", [&](const MatrixModel::Send&) -> std::optional<json::Value> {
                json::Value v = json::Value::object();
                v["tap"] = value;
                return v;
            });
        }));
    }
    // Level and pan: the strip's fader and pan control (a drag is one undo
    // step and records automation like the fader's, gui.md §4.6).
    volume_ = &row.emplace<ui::Fader>("routing.volume", ui::Fader::Orientation::Horizontal);
    volume_->layout.width = ui::SizeRule::fixed(150);
    volume_->layout.height = ui::SizeRule::fixed(20);
    volume_->onChange = [this](double db, ui::Gesture g) { level(db, g); };
    volumeText_ =
        &row.emplace<ui::Label>("routing.volume.value", "", ui::Font::Mono, "caption", "ink-quiet");
    volumeText_->layout.width = ui::SizeRule::fixed(58);
    pan_ = &row.emplace<ui::PanControl>("routing.pan");
    pan_->layout.width = ui::SizeRule::fixed(90);
    pan_->layout.height = ui::SizeRule::fixed(18);
    pan_->onChange = [this](double pan, ui::Gesture g) { panTo(pan, g); };
    panText_ =
        &row.emplace<ui::Label>("routing.pan.value", "", ui::Font::Mono, "caption", "ink-quiet");
    panText_->layout.width = ui::SizeRule::fixed(40);
    // Channels: how each send's output lands on its destination (route.set
    // {channels, mono}); a menu of the stereo choices.
    channels_ = &row.emplace<ui::Button>("routing.channels", "L/R", [this] {
        const MatrixModel::Send* shown = sendById(selectedSend().value_or(""));
        if (!shown || !channels_) return;
        auto channelsOf = [&](const std::string& id) {
            const auto tid = model::Id::parse(id);
            const model::Track* t = tid ? app_.head().tracks.find(*tid) : nullptr;
            return t ? std::max(2, t->channels) : 2;
        };
        struct Choice {
            std::string label;
            std::vector<int> from, to;
            bool mono;
            int toPair = 0;  // a pair of the destination's: `from` is each send's own pair
        };
        static const Choice kChoices[] = {{"L/R → L/R", {}, {}, false},
                                          {"Swap: L/R → R/L", {1, 2}, {2, 1}, false},
                                          {"Left to both", {1, 1}, {1, 2}, false},
                                          {"Right to both", {2, 2}, {1, 2}, false},
                                          {"Left only", {1}, {1}, false},
                                          {"Right only", {2}, {2}, false},
                                          {"Mono (sum)", {}, {}, true}};
        // The pair a send leaves from stays (its row, ADR-0021): the stereo
        // choices for the main pair, then the destination's pairs as far as
        // the shown send's has them (its auxiliary channels, ADR-0020). Each
        // choice goes to the selected sends from the same pair whose
        // destinations have those channels.
        const int a = MatrixModel::sourcePair(*shown);
        const int width = a + 1 <= channelsOf(shown->from) ? 2 : 1;
        std::vector<Choice> choices;
        if (a == 1) choices.assign(std::begin(kChoices), std::end(kChoices));
        for (int b = 1; b + 1 <= channelsOf(shown->to); b += 2) {
            if (a == 1 && b == 1) continue;
            choices.push_back(
                {MatrixModel::pairText(a, width) + " → " + MatrixModel::pairText(b, 2),
                 {},
                 {b, b + 1},
                 false,
                 b});
        }
        std::vector<ui::MenuItem> items;
        for (const Choice& c : choices) {
            ui::MenuItem m;
            m.label = c.label;
            m.run = [this, c, a, channelsOf] {
                (void)setSends(
                    "Send channels", [&](const MatrixModel::Send& x) -> std::optional<json::Value> {
                        if (MatrixModel::sourcePair(x) != a) return std::nullopt;
                        std::vector<int> from = c.from, to = c.to;
                        if (c.toPair) {
                            if (c.toPair + 1 > channelsOf(x.to)) return std::nullopt;
                            from = a + 1 <= channelsOf(x.from) ? std::vector<int>{a, a + 1}
                                                               : std::vector<int>{a, a};
                        }
                        json::Value v = json::Value::object(), map = json::Value::object(),
                                    f = json::Value::array(), t = json::Value::array();
                        for (int n : from) f.push_back(static_cast<std::int64_t>(n));
                        for (int n : to) t.push_back(static_cast<std::int64_t>(n));
                        map["from"] = std::move(f);
                        map["to"] = std::move(t);
                        v["channels"] = std::move(map);
                        v["mono"] = c.mono;
                        return v;
                    });
            };
            items.push_back(std::move(m));
        }
        const ui::Rect b = channels_->bounds();
        root().openPopup(ui::makeMenu("routing.channels.menu", std::move(items),
                                      [this] { root().closePopup(); }),
                         b, ui::Placement::Below);
    });
    channels_->layout.width = ui::SizeRule::fixed(96);
    // Polarity and mute: on for all unless all are on, then off for all.
    phase_ = &row.emplace<ui::Button>("routing.phase", "ø", [this] {
        bool all = true;
        for (const auto& id : selectedSends())
            if (const auto* x = sendById(id)) all = all && x->phaseInvert;
        (void)setSends("Send polarity",
                       [&](const MatrixModel::Send&) -> std::optional<json::Value> {
                           json::Value v = json::Value::object();
                           v["phase_invert"] = !all;
                           return v;
                       });
    });
    phase_->layout.width = ui::SizeRule::fixed(44);
    mute_ = &row.emplace<ui::Button>("routing.mute", "Mute", [this] {
        bool all = true;
        for (const auto& id : selectedSends())
            if (const auto* x = sendById(id)) all = all && x->mute;
        (void)setSends("Send mute", [&](const MatrixModel::Send&) -> std::optional<json::Value> {
            json::Value v = json::Value::object();
            v["mute"] = !all;
            return v;
        });
    });
    status_ = &panel->emplace<ui::Label>("routing.status", "", ui::Font::Ui, "small", "ink-quiet");
    status_->layout.height = ui::SizeRule::fixed(18);
    panel_.open(
        std::move(panel),
        [this] {
            grid_ = nullptr;
            status_ = sendTitle_ = volumeText_ = panText_ = nullptr;
            details_ = nullptr;
            volume_ = nullptr;
            pan_ = nullptr;
            add_ = remove_ = mute_ = nullptr;
            channels_ = phase_ = nullptr;
            taps_.clear();
            selection_.clear();
            anchor_.reset();
            anchorRow_.reset();
            anchorCol_.reset();
        },
        anchor);
    if (auto r = reload(); !r && status_) status_->setText(r.error().message);
    if (status_) status_->setText(status());
}

std::string RoutingMatrix::channelsText(const MatrixModel::Send& x) {
    if (x.mono) return "Mono";
    using V = std::vector<int>;
    if (x.mapFrom.empty()) return "L/R";
    if (x.mapFrom == V{1, 2} && x.mapTo == V{2, 1}) return "Swap R/L";
    if (x.mapFrom == V{1, 1} && x.mapTo == V{1, 2}) return "L to both";
    if (x.mapFrom == V{2, 2} && x.mapTo == V{1, 2}) return "R to both";
    if (x.mapFrom == V{1} && x.mapTo == V{1}) return "L only";
    if (x.mapFrom == V{2} && x.mapTo == V{2}) return "R only";
    if (x.mapFrom.size() == 2 && x.mapTo.size() == 2 && x.mapFrom[1] == x.mapFrom[0] + 1 &&
        x.mapTo[1] == x.mapTo[0] + 1)  // a pair to a pair (auxiliary channels, ADR-0020)
        return std::to_string(x.mapFrom[0]) + "/" + std::to_string(x.mapFrom[1]) + "\u2192" +
               std::to_string(x.mapTo[0]) + "/" + std::to_string(x.mapTo[1]);
    if (x.mapFrom.size() == 2 && x.mapTo.size() == 2 && x.mapFrom[0] == x.mapFrom[1] &&
        x.mapTo[1] == x.mapTo[0] + 1)  // one channel to a pair (a last odd output)
        return std::to_string(x.mapFrom[0]) + "\u2192" + std::to_string(x.mapTo[0]) + "/" +
               std::to_string(x.mapTo[1]);
    return "Custom map";
}

std::string RoutingMatrix::status() const {
    return std::to_string(model_.sends.size()) +
           " sends · click to select (Ctrl, Shift, a row or column name: more), "
           "double-click to send";
}

RoutingMatrix::Cell RoutingMatrix::cellAt(std::size_t row, std::size_t col) const {
    return {model_.sources[row].id, model_.sources[row].pair, model_.destinations[col].id};
}

std::optional<std::pair<std::size_t, std::size_t>> RoutingMatrix::locate(const Cell& c) const {
    std::optional<std::size_t> row, col;
    for (std::size_t i = 0; i < model_.sources.size() && !row; ++i)
        if (model_.sources[i].id == c.source && model_.sources[i].pair == c.pair) row = i;
    for (std::size_t i = 0; i < model_.destinations.size() && !col; ++i)
        if (model_.destinations[i].id == c.destination) col = i;
    if (!row || !col) return std::nullopt;
    return std::pair{*row, *col};
}

bool RoutingMatrix::parentCell(std::size_t row, std::size_t col) const {
    return model_.destinations[col].kind == "track" && model_.sources[row].pair == 1 &&
           model_.sources[row].parent == model_.destinations[col].id;
}

bool RoutingMatrix::isSelected(std::size_t row, std::size_t col) const {
    return row < model_.sources.size() && col < model_.destinations.size() &&
           selection_.contains(cellAt(row, col));
}

const MatrixModel::Send* RoutingMatrix::sendById(const std::string& id) const {
    for (const auto& x : model_.sends)
        if (x.id == id) return &x;
    return nullptr;
}

std::vector<std::string> RoutingMatrix::selectedSends() const {
    std::vector<std::string> out;
    for (const Cell& c : selection_)
        if (const auto at = locate(c))
            for (const auto* x : model_.sendsAt(at->first, at->second))
                if (std::find(out.begin(), out.end(), x->id) == out.end()) out.push_back(x->id);
    return out;
}

std::optional<std::string> RoutingMatrix::selectedSend() const {
    if (anchor_ && selection_.contains(*anchor_))
        if (const auto at = locate(*anchor_))
            if (const auto sends = model_.sendsAt(at->first, at->second); !sends.empty())
                return sends.front()->id;
    const auto all = selectedSends();
    if (all.empty()) return std::nullopt;
    return all.front();
}

void RoutingMatrix::select(std::set<Cell> cells, std::optional<Cell> anchor) {
    selection_ = std::move(cells);
    if (anchor) anchor_ = std::move(anchor);
    refresh();
}

Status RoutingMatrix::clickCell(std::size_t row, std::size_t col, unsigned mods) {
    if (row >= model_.sources.size() || col >= model_.destinations.size())
        return Error("gui.routing", "no such cell");
    const Cell c = cellAt(row, col);
    anchorRow_.reset();
    anchorCol_.reset();
    std::set<Cell> next = mods & ui::kCtrl ? selection_ : std::set<Cell>{};
    if ((mods & ui::kShift) && anchor_) {
        // The rectangle from the anchor to here.
        if (const auto from = locate(*anchor_)) {
            for (std::size_t r = std::min(from->first, row); r <= std::max(from->first, row); ++r)
                for (std::size_t k = std::min(from->second, col); k <= std::max(from->second, col);
                     ++k)
                    next.insert(cellAt(r, k));
            select(std::move(next), std::nullopt);
            return {};
        }
    }
    if ((mods & ui::kCtrl) && selection_.contains(c))
        next.erase(c);
    else
        next.insert(c);
    select(std::move(next), c);
    return {};
}

void RoutingMatrix::clickRow(std::size_t row, unsigned mods) {
    if (row >= model_.sources.size()) return;
    std::set<Cell> next = mods & ui::kCtrl ? selection_ : std::set<Cell>{};
    std::size_t first = row, last = row;
    if ((mods & ui::kShift) && anchorRow_) {
        first = std::min(*anchorRow_, row);
        last = std::max(*anchorRow_, row);
    } else {
        anchorRow_ = row;
    }
    anchorCol_.reset();
    bool all = (mods & ui::kCtrl) && !(mods & ui::kShift);  // Ctrl on a whole row: off
    for (std::size_t k = 0; k < model_.destinations.size(); ++k)
        all = all && selection_.contains(cellAt(row, k));
    for (std::size_t r = first; r <= last; ++r)
        for (std::size_t k = 0; k < model_.destinations.size(); ++k) {
            if (all)
                next.erase(cellAt(r, k));
            else
                next.insert(cellAt(r, k));
        }
    select(std::move(next),
           model_.destinations.empty() ? std::nullopt : std::optional(cellAt(row, 0)));
}

void RoutingMatrix::clickColumn(std::size_t col, unsigned mods) {
    if (col >= model_.destinations.size()) return;
    std::set<Cell> next = mods & ui::kCtrl ? selection_ : std::set<Cell>{};
    std::size_t first = col, last = col;
    if ((mods & ui::kShift) && anchorCol_) {
        first = std::min(*anchorCol_, col);
        last = std::max(*anchorCol_, col);
    } else {
        anchorCol_ = col;
    }
    anchorRow_.reset();
    bool all = (mods & ui::kCtrl) && !(mods & ui::kShift);
    for (std::size_t r = 0; r < model_.sources.size(); ++r)
        all = all && selection_.contains(cellAt(r, col));
    for (std::size_t k = first; k <= last; ++k)
        for (std::size_t r = 0; r < model_.sources.size(); ++r) {
            if (all)
                next.erase(cellAt(r, k));
            else
                next.insert(cellAt(r, k));
        }
    select(std::move(next), model_.sources.empty() ? std::nullopt : std::optional(cellAt(0, col)));
}

void RoutingMatrix::selectAll() {
    std::set<Cell> next;
    for (std::size_t r = 0; r < model_.sources.size(); ++r)
        for (std::size_t k = 0; k < model_.destinations.size(); ++k) next.insert(cellAt(r, k));
    anchorRow_.reset();
    anchorCol_.reset();
    select(std::move(next), std::nullopt);
}

namespace {

// route.add's arguments for a cell's pair to its destination.
json::Value addArgs(const MatrixModel::Source& s, const MatrixModel::Destination& d) {
    json::Value args = json::Value::object();
    args["from"] = s.id;
    args[d.kind == "key" ? "fx" : "to"] = d.id;
    if (s.pair != 1) {  // an auxiliary pair to the destination's main pair (ADR-0021)
        json::Value map = json::Value::object(), from = json::Value::array(),
                    to = json::Value::array();
        from.push_back(static_cast<std::int64_t>(s.pair));
        from.push_back(static_cast<std::int64_t>(s.width == 2 ? s.pair + 1 : s.pair));
        to.push_back(std::int64_t{1});
        to.push_back(std::int64_t{2});
        map["from"] = std::move(from);
        map["to"] = std::move(to);
        args["channels"] = std::move(map);
    }
    return args;
}

}  // namespace

Status RoutingMatrix::doubleClickCell(std::size_t row, std::size_t col) {
    if (row >= model_.sources.size() || col >= model_.destinations.size())
        return Error("gui.routing", "no such cell");
    select({cellAt(row, col)}, cellAt(row, col));
    if (!model_.sendsAt(row, col).empty()) return {};
    // The parent's cell is the implicit parent send: on or off.
    if (parentCell(row, col)) {
        json::Value args = json::Value::object();
        args["track"] = model_.sources[row].id;
        args["parent_send"] = !model_.sources[row].parentSend;
        auto r = exec("track.set", std::move(args));
        if (!r) return std::move(r).error();
        return reload();
    }
    if (const std::string why = model_.blocked(row, col); !why.empty()) {
        if (status_) status_->setText("Not allowed: " + why);
        return Error("route.strip_type", why);
    }
    auto r = exec("route.add", addArgs(model_.sources[row], model_.destinations[col]));
    if (!r) return std::move(r).error();
    return reload();
}

Status RoutingMatrix::addSelected() {
    std::vector<std::pair<std::string, json::Value>> edits;
    for (const Cell& c : selection_) {
        const auto at = locate(c);
        if (!at || !model_.sendsAt(at->first, at->second).empty()) continue;
        const auto& s = model_.sources[at->first];
        if (parentCell(at->first, at->second)) {
            if (s.parentSend) continue;
            json::Value args = json::Value::object();
            args["track"] = s.id;
            args["parent_send"] = true;
            edits.emplace_back("track.set", std::move(args));
        } else if (model_.blocked(at->first, at->second).empty()) {
            edits.emplace_back("route.add", addArgs(s, model_.destinations[at->second]));
        }
    }
    if (edits.empty()) return {};
    auto r = dispatcher_.transact(edits.size() == 1 ? "Add send" : "Add sends", kOrigin,
                                  [&]() -> Result<json::Value> {
                                      for (auto& [command, args] : edits)
                                          if (auto e = exec(command, args); !e) return e;
                                      return json::Value::object();
                                  });
    if (!r) return std::move(r).error();
    return reload();
}

Status RoutingMatrix::removeSelected() {
    std::vector<std::pair<std::string, json::Value>> edits;
    for (const auto& id : selectedSends()) {
        json::Value args = json::Value::object();
        args["route"] = id;
        edits.emplace_back("route.remove", std::move(args));
    }
    for (const Cell& c : selection_)
        if (const auto at = locate(c);
            at && parentCell(at->first, at->second) && model_.sources[at->first].parentSend) {
            json::Value args = json::Value::object();
            args["track"] = c.source;
            args["parent_send"] = false;
            edits.emplace_back("track.set", std::move(args));
        }
    if (edits.empty()) return {};
    auto r = dispatcher_.transact(edits.size() == 1 ? "Remove send" : "Remove sends", kOrigin,
                                  [&]() -> Result<json::Value> {
                                      for (auto& [command, args] : edits)
                                          if (auto e = exec(command, args); !e) return e;
                                      return json::Value::object();
                                  });
    if (!r) return std::move(r).error();
    return reload();
}

Status RoutingMatrix::setSends(
    const std::string& label,
    const std::function<std::optional<json::Value>(const MatrixModel::Send&)>& each,
    const std::string& gesture) {
    cmd::Origin origin = kOrigin;
    origin.gesture = gesture;
    std::vector<json::Value> edits;
    for (const auto& id : selectedSends())
        if (const auto* x = sendById(id))
            if (auto args = each(*x)) {
                (*args)["route"] = id;
                edits.push_back(std::move(*args));
            }
    if (edits.empty()) return {};
    auto apply = [&]() -> Result<json::Value> {
        for (auto& args : edits)
            if (auto e = exec("route.set", args, &origin); !e) return e;
        return json::Value::object();
    };
    auto r = edits.size() == 1 ? apply() : dispatcher_.transact(label, origin, apply);
    if (!r) return std::move(r).error();
    return reload();
}

bool RoutingMatrix::learnAt(ui::Point p) {
    std::vector<std::pair<const ui::Node*, std::string>> controls = {
        {volume_, "volume"}, {pan_, "pan"}, {mute_, "mute"}, {phase_, "phase_invert"}};
    const char* taps[] = {"tap:post_fader", "tap:pre_fader", "tap:pre_fx"};
    for (std::size_t i = 0; i < taps_.size() && i < 3; ++i) controls.push_back({taps_[i], taps[i]});
    StripControl control;
    for (const auto& id : selectedSends())
        if (auto parsed = model::Id::parse(id)) control.objects.push_back(*parsed);
    if (control.objects.empty()) return false;
    for (const auto& [node, param] : controls)
        if (node && node->bounds().contains(p)) {
            control.param = param;
            openStripLearnMenu(root(), actions_, std::move(control), {p.x, p.y, 1, 1});
            return true;
        }
    return false;
}

void RoutingMatrix::level(double db, ui::Gesture g) {
    if (volumeText_) volumeText_->setText(dbText(db));
    const auto shown = selectedSend();
    if (g == ui::Gesture::Begin) {
        ++gesture_;
        dragStart_.clear();
        for (const auto& id : selectedSends())
            if (const auto* x = sendById(id)) dragStart_[id] = {x->volumeDb, x->pan};
        return;
    }
    if (g == ui::Gesture::End || !shown) return;
    if (auto id = model::Id::parse(*shown)) actions_.touchSend(*id, "volume");
    // A drag moves each send by the same dB from where it began (silence
    // stays put unless the shown send goes there); a one-shot sets them all.
    const bool drag = g == ui::Gesture::Move && dragStart_.contains(*shown);
    const double ref = drag ? dragStart_.at(*shown).first : 0;
    (void)setSends(
        "Send level",
        [&](const MatrixModel::Send& x) -> std::optional<json::Value> {
            json::Value v = json::Value::object();
            double to = db;
            if (drag && dragStart_.contains(x.id) && x.id != *shown && db > ui::kFaderSilenceDb) {
                const double start = dragStart_.at(x.id).first;
                to = ref <= ui::kFaderSilenceDb || start <= ui::kFaderSilenceDb
                         ? start
                         : std::clamp(start + db - ref, ui::kFaderSilenceDb, 12.0);
            }
            v["volume_db"] = to;
            return v;
        },
        drag ? "routing.volume:" + std::to_string(gesture_) : "");
}

void RoutingMatrix::panTo(double pan, ui::Gesture g) {
    if (panText_) panText_->setText(panText(pan));
    const auto shown = selectedSend();
    if (g == ui::Gesture::Begin) {
        ++gesture_;
        dragStart_.clear();
        for (const auto& id : selectedSends())
            if (const auto* x = sendById(id)) dragStart_[id] = {x->volumeDb, x->pan};
        return;
    }
    if (g == ui::Gesture::End || !shown) return;
    if (auto id = model::Id::parse(*shown)) actions_.touchSend(*id, "pan");
    const bool drag = g == ui::Gesture::Move && dragStart_.contains(*shown);
    const double ref = drag ? dragStart_.at(*shown).second : 0;
    (void)setSends(
        "Send pan",
        [&](const MatrixModel::Send& x) -> std::optional<json::Value> {
            json::Value v = json::Value::object();
            v["pan"] = drag && dragStart_.contains(x.id)
                           ? std::clamp(dragStart_.at(x.id).second + pan - ref, -100.0, 100.0)
                           : pan;
            return v;
        },
        drag ? "routing.pan:" + std::to_string(gesture_) : "");
}

void RoutingMatrix::refresh() {
    if (grid_) grid_->invalidate();
    if (!details_) return;
    const auto sends = selectedSends();
    const MatrixModel::Send* x = sendById(selectedSend().value_or(""));
    bool addable = false, removable = !sends.empty();
    for (const Cell& c : selection_)
        if (const auto at = locate(c)) {
            const bool parent = parentCell(at->first, at->second);
            const bool on = model_.sources[at->first].parentSend;
            if (model_.sendsAt(at->first, at->second).empty() &&
                (parent ? !on : model_.blocked(at->first, at->second).empty()))
                addable = true;
            if (parent && on) removable = true;
        }
    add_->enabled = addable;
    remove_->enabled = removable;
    for (const auto& n : details_->children().back()->children())  // the controls' row
        n->enabled = x != nullptr;
    auto nameOf = [&](const std::string& id) {
        for (const auto& s : model_.sources)
            if (s.id == id) return s.name.empty() ? id : s.name;
        for (const auto& d : model_.destinations)
            if (d.id == id) return d.name.empty() ? id : d.name;
        return id;
    };
    if (selection_.empty())
        sendTitle_->setText("Select cells: click, Ctrl or Shift+click, or a row or column name");
    else if (selection_.size() == 1 && !x) {
        const Cell& c = *selection_.begin();
        sendTitle_->setText(nameOf(c.source) + " → " + nameOf(c.destination) +
                            ": no send (double-click or Add)");
    } else if (sends.size() <= 1 && x) {
        std::string from = nameOf(x->from);
        for (const auto& s : model_.sources)  // an auxiliary pair's send: its row's name
            if (s.id == x->from && s.pair != 1 && s.pair == MatrixModel::sourcePair(*x))
                from = s.strip + " " + s.name;
        sendTitle_->setText(from + " → " + nameOf(x->to) + (x->mute ? " (muted)" : ""));
    } else {
        sendTitle_->setText(std::to_string(selection_.size()) + " cells selected, " +
                            std::to_string(sends.size()) + " sends");
    }
    if (!x) {  // nothing to edit: neutral controls
        volume_->setValue(0);
        pan_->setValue(0);
        volumeText_->setText("");
        panText_->setText("");
        const char* plain[] = {"Post", "Pre", "Pre-FX"};
        for (std::size_t i = 0; i < taps_.size(); ++i) taps_[i]->setText(plain[i]);
        mute_->setText("Mute");
        channels_->setText("L/R");
        phase_->setText("\u00f8");
        return;
    }
    // The shown send's values; the tap marked when every selected send has it.
    const char* names[] = {"post_fader", "pre_fader", "pre_fx"};
    const char* labels[] = {"Post", "Pre", "Pre-FX"};
    bool allMuted = true, allInverted = true;
    for (const auto& id : sends)
        if (const auto* y = sendById(id)) {
            allMuted = allMuted && y->mute;
            allInverted = allInverted && y->phaseInvert;
        }
    for (std::size_t i = 0; i < taps_.size(); ++i) {
        bool every = true;
        for (const auto& id : sends)
            if (const auto* y = sendById(id)) every = every && y->tap == names[i];
        taps_[i]->setText(std::string(every ? "• " : "") + labels[i]);
    }
    if (!volume_->dragging()) {
        volume_->setValue(x->volumeDb);
        volumeText_->setText(dbText(x->volumeDb));
    }
    pan_->setValue(x->pan);
    panText_->setText(panText(x->pan));
    mute_->setText(allMuted ? "Unmute" : "Mute");
    channels_->setText(channelsText(*x));
    phase_->setText(allInverted ? "ø on" : "ø");
}

void RoutingMatrix::sync() {
    if (isOpen() &&
        (app_.projectGeneration() != projectGeneration_ || app_.head().version != version_))
        (void)reload();
}

}  // namespace supple::gui
