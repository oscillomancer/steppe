// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "midi_event_list.h"
#include "supple/harmony/harmony.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "midi_editor.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {
constexpr std::size_t kColumns = 6;

ui::Rect cellRect(ui::Rect row, std::size_t column) {
    const float width = (row.w - ui::ScrollBar::kThickness) / kColumns;
    return {row.x + static_cast<float>(column) * width, row.y, width, row.h};
}

bool integer(std::string_view text, int& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool parts(std::string_view text, std::vector<int>& out, std::size_t count) {
    while (true) {
        const auto dot = text.find('.');
        int value = 0;
        if (!integer(text.substr(0, dot), value)) return false;
        out.push_back(value);
        if (dot == std::string_view::npos) break;
        text.remove_prefix(dot + 1);
    }
    return out.size() == count;
}

std::string pitchName(int pitch) { return harmony::spell(pitch); }

bool pitchNumber(std::string_view text, int& value) {
    if (integer(text, value)) return value >= 0 && value <= 127;
    if (text.size() < 2) return false;
    const auto note = std::string_view("C D EF G A B").find(text.front());
    if (note == std::string_view::npos || text.front() == ' ') return false;
    text.remove_prefix(1);
    int sharp = 0;  // a sharp is +1, a flat -1: the names shown are the default spelling (Bb)
    if (text.front() == '#' || text.front() == 'b') {
        sharp = text.front() == '#' ? 1 : -1;
        text.remove_prefix(1);
    }
    int octave = 0;
    if (!integer(text, octave) || octave < -1 || octave > 9) return false;
    value = (octave + 1) * 12 + static_cast<int>(note) + sharp;
    return value >= 0 && value <= 127;
}

json::Value noteOp(const char* name, std::size_t index) {
    auto op = json::Value::object();
    op["op"] = name;
    auto notes = json::Value::array();
    notes.push_back(static_cast<std::int64_t>(index));
    op["notes"] = std::move(notes);
    return op;
}

// The toolkit handles scrolling/navigation; this subclass adds cell hit
// testing, modifier selection, Delete and one reusable inline text field.
class EventRows : public ui::List {
  public:
    EventRows() : List("midi.list.rows") {
        clipsChildren = true;
        field = &emplace<ui::TextField>("midi.list.edit");
        field->visible = false;
    }
    ui::TextField* field;
    std::size_t editRow = 0, editColumn = 0;
    unsigned mods = 0;
    std::function<void(std::size_t, std::size_t)> editCell;
    std::function<void()> cancelEdit;
    void placeField(const ui::Canvas& canvas) {
        auto r = bounds();
        r.y += static_cast<float>(editRow) * rowHeight - scroll();
        r.h = rowHeight;
        field->arrange(canvas, cellRect(r, editColumn));
    }
    void arrange(const ui::Canvas& c, const ui::Rect& r) override {
        List::arrange(c, r);
        placeField(c);
    }
    bool onPointer(const ui::PointerEvent& e) override {
        mods = e.mods;
        if (e.type == ui::PointerEvent::Type::Down || e.type == ui::PointerEvent::Type::Wheel)
            if (cancelEdit) cancelEdit();
        const float rowPosition = (e.pos.y - bounds().y + scroll()) / rowHeight;
        const auto row = static_cast<std::size_t>(std::max(0.0f, rowPosition));
        const bool rowHit = bounds().contains(e.pos) && rowPosition >= 0 && row < count() &&
                            e.pos.x < bounds().right() - ui::ScrollBar::kThickness;
        const bool handled = List::onPointer(e);
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1 && e.clicks >= 2 && rowHit) {
            const float width = (bounds().w - ui::ScrollBar::kThickness) / kColumns;
            if (width > 0) {
                const auto column =
                    static_cast<std::size_t>(std::max(0.0f, e.pos.x - bounds().x) / width);
                if (column < kColumns && editCell) editCell(row, column);
            }
        }
        return handled;
    }
    bool onKey(const ui::KeyEvent& e) override {
        mods = e.mods;
        return List::onKey(e);  // Delete is the keymap's (ui.midi.notes.delete)
    }
};

class Columns : public ui::Node {
  public:
    Columns() : Node("midi.list.columns") { layout.height = ui::SizeRule::fixed(24); }
    // True while the take follows: the rows are its written notes, and the
    // header says so at its right (gui.md §4.7).
    std::function<bool()> written;
    void paint(ui::Canvas& c) override {
        static constexpr const char* labels[] = {"Position",   "Type",        "Length",
                                                 "Pitch / CC", "Vel / value", "Channel"};
        for (std::size_t i = 0; i < kColumns; ++i)
            c.text(cellRect(bounds(), i), labels[i], ui::Font::UiBold, c.theme().type("caption"),
                   c.theme().color("ink-quiet"));
        if (written && written()) {
            const ui::Rect b = bounds();
            c.text({b.right() - 68, b.y, 60, b.h}, "written", ui::Font::Ui,
                   c.theme().type("caption"), c.theme().color("ink-quiet"),
                   ui::kRight | ui::kMiddle);
        }
    }
};
}  // namespace

MidiEventList::MidiEventList(ui::Root& root, MidiEditor& editor, engine::EngineApp& app)
    : root_(root), editor_(editor), app_(app) {}

std::unique_ptr<ui::Node> MidiEventList::build() {
    auto panel = std::make_unique<ui::Node>("midi.list");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fraction();
    panel->visible = false;
    auto& columns = panel->emplace<Columns>();
    columns.written = [this] { return editor_.state().following; };
    columns_ = &columns;
    auto& rows = panel->emplace<EventRows>();
    list_ = &rows;
    field_ = rows.field;
    rows.rowHeight = 26;
    rows.onSelect = [this, &rows](std::size_t row) { select(row, rows.mods); };
    rows.editCell = [this](std::size_t row, std::size_t column) {
        begin(row, static_cast<Column>(column));
    };
    rows.cancelEdit = [this] { cancel(); };
    rows.paintRow = [this](ui::Canvas& c, std::size_t row, const ui::Rect& r, bool) {
        c.fillRect(r, c.theme().color(selected(row) ? "selection" : "ground"));
        const auto text = rowText(row);
        // Keep header, pointer hit testing and painting on the same column grid.
        ui::Rect full = r;
        full.w = list_->bounds().w;
        for (std::size_t i = 0; i < kColumns; ++i)
            c.text(cellRect(full, i), text[i], ui::Font::Mono, c.theme().type("caption"),
                   c.theme().color("ink"));
    };
    field_->onSubmit = [this](const std::string& text) { submit(text); };
    field_->onCancel = [this] {
        cancel();
        root_.focus(list_);
    };
    auto& footer = panel->emplace<ui::Node>("midi.list.footer");
    footer.layout.dir = ui::Layout::Dir::Row;
    footer.layout.height = ui::SizeRule::fixed(28);
    footer.layout.gap = 8;
    footer.emplace<ui::Button>("midi.list.add", "+ Note", [this] { add(); });
    status_ =
        &footer.emplace<ui::Label>("midi.list.status", "", ui::Font::Ui, "caption", "ink-quiet");
    status_->layout.width = ui::SizeRule::fraction();
    return panel;
}

void MidiEventList::status(std::string text) {
    if (status_) status_->setText(std::move(text));
}

void MidiEventList::cancel() {
    editing_.reset();
    if (field_) {
        root_.releaseWithin(field_);
        field_->visible = false;
        list_->invalidate();
    }
}

void MidiEventList::sync() {
    const auto& s = app_.head();
    if (app_.projectGeneration() == projectGeneration_ && item_ == editor_.state().item &&
        version_ == s.version)
        return;
    cancel();
    item_ = editor_.state().item;
    if (columns_) columns_->invalidate();  // "written" comes and goes with following
    projectGeneration_ = app_.projectGeneration();
    version_ = s.version;
    rows_.clear();
    frameShift_ = 0;
    selectedCc_.clear();
    anchor_.reset();
    const auto owner = midiOwner(s, item_);
    const model::Take* take = owner ? owner->take : nullptr;
    const model::MidiSource* source = owner ? owner->source : nullptr;
    if (source) {
        const auto& seq = source->sequence;
        const double rate = take->playrate > 0 ? take->playrate : 1;
        tickBeat_ = 1.0 / (seq.ppq * rate);
        // Rows sit on the roll's axis, so a looped item's events show where
        // they play; writes take frameShift_ off for midi.edit's item beats.
        frameShift_ = rollFrameShift(*owner);
        auto beatOf = [&](std::int64_t tick) {
            return (static_cast<double>(tick) / seq.ppq - take->startOffset) / rate + frameShift_;
        };
        for (std::size_t i = 0; i < seq.notes.size(); ++i) {
            const auto& n = seq.notes[i];
            rows_.push_back({false, i, beatOf(n.start), static_cast<double>(n.length) * tickBeat_,
                             n.pitch, n.velocity, n.channel + 1});
        }
        for (std::size_t i = 0; i < seq.controls.size(); ++i) {
            const auto& cc = seq.controls[i];
            if (cc.kind == midi::ControlKind::CC)
                rows_.push_back({true, i, beatOf(cc.tick), 0, cc.number, cc.value, cc.channel + 1});
        }
        std::stable_sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
            if (a.beat != b.beat) return a.beat < b.beat;
            if (a.cc != b.cc) return a.cc < b.cc;
            if (a.pitch != b.pitch) return a.pitch < b.pitch;
            return a.channel < b.channel;
        });
    }
    list_->setCount(rows_.size());
    refreshSelection();
}

bool MidiEventList::selected(std::size_t row) const {
    const Row& r = rows_[row];
    return r.cc ? selectedCc_.contains(r.index) : editor_.state().selected.contains(r.index);
}

void MidiEventList::refreshSelection() {
    list_->select(std::nullopt);
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (selected(i)) {
            list_->select(i);
            break;
        }
    list_->invalidate();
}

void MidiEventList::activate(bool shown) {
    cancel();
    if (!shown) root_.releaseWithin(list_);
    if (shown) {
        sync();
        refreshSelection();
        root_.focus(list_);
    }
}

void MidiEventList::select(std::size_t row, unsigned mods) {
    if (row >= rows_.size()) return;
    auto& notes = editor_.state().selected;
    if (!(mods & ui::kCtrl)) {
        notes.clear();
        selectedCc_.clear();
    }
    auto set = [&](std::size_t i) {
        const auto& r = rows_[i];
        auto& selection = r.cc ? selectedCc_ : notes;
        if ((mods & ui::kCtrl) && !(mods & ui::kShift) && selection.contains(r.index))
            selection.erase(r.index);
        else
            selection.insert(r.index);
    };
    if ((mods & ui::kShift) && anchor_) {
        for (std::size_t i = std::min(*anchor_, row); i <= std::max(*anchor_, row); ++i) set(i);
    } else {
        set(row);
        anchor_ = row;
    }
    list_->invalidate();
}

MidiEventList::RowText MidiEventList::rowText(std::size_t row) const {
    if (row >= rows_.size()) return {};
    const Row& r = rows_[row];
    const auto& map = *app_.head().timeMap;
    auto p = map.barBeat(editor_.state().itemPosition + r.beat);
    auto ticks = std::llround(p.fraction * 960);
    if (ticks == 960) {
        p = map.barBeat(map.beatOf({p.bar, p.beat + 1, 0, 0}));
        ticks = 0;
    }
    const auto length = std::llround(r.length * 960);
    return {std::to_string(p.bar) + "." + std::to_string(p.beat) + "." + std::to_string(ticks),
            r.cc ? "CC " + std::to_string(r.pitch) : "Note",
            r.cc ? "" : std::to_string(length / 960) + "." + std::to_string(length % 960),
            r.cc ? std::to_string(r.pitch) : pitchName(r.pitch),
            std::to_string(r.value),
            std::to_string(r.channel)};
}

void MidiEventList::begin(std::size_t row, Column column) {
    if (row >= rows_.size() || column == Column::Type || column == Column::Channel ||
        (rows_[row].cc && column == Column::Length))
        return;
    editing_ = std::pair{row, column};
    auto* rows = static_cast<EventRows*>(list_);
    rows->editRow = row;
    rows->editColumn = static_cast<std::size_t>(column);
    field_->setText(rowText(row)[rows->editColumn]);
    field_->selectAll();
    field_->visible = true;
    root_.relayout();
    root_.focus(field_);
    field_->invalidate();
    status("");
}

void MidiEventList::submit(const std::string& text) {
    if (!editing_) return;
    if (app_.projectGeneration() != projectGeneration_ || app_.head().version != version_ ||
        editor_.state().item != item_) {
        cancel();
        status("Events changed; edit the cell again.");
        return;
    }
    const auto [index, column] = *editing_;
    const Row r = rows_[index];
    auto ops = json::Value::array();
    auto op = noteOp("move", r.index);
    double beat = r.beat;
    int pitch = r.pitch, value = r.value;
    auto bad = [&](const char* hint) { status(hint); };
    if (column == Column::Position) {
        std::vector<int> p;
        if (!parts(text, p, 3) || p[0] < 1 || p[0] > 1000000 || p[1] < 1 || p[2] < 0 ||
            p[2] >= 960 || p[1] > app_.head().timeMap->meterAt(p[0]).numerator) {
            bad("Use bar.beat.tick (bars/beats from 1, ticks 0–959).");
            return;
        }
        beat = app_.head().timeMap->beatOf({p[0], p[1], p[2] / 960.0, 0}) -
               editor_.state().itemPosition;
        op["beats"] = beat - r.beat;
    } else if (column == Column::Length) {
        std::vector<int> p;
        if (!parts(text, p, 2) || p[0] < 0 || p[1] < 0 || p[1] >= 960 || (p[0] == 0 && p[1] == 0)) {
            bad("Use beats.ticks (positive length, ticks 0–959).");
            return;
        }
        op["op"] = "resize";
        op["length"] = p[0] + p[1] / 960.0;
    } else if (column == Column::Pitch) {
        if (r.cc ? (!integer(text, pitch) || pitch < 0 || pitch > 127)
                 : !pitchNumber(text, pitch)) {
            bad(r.cc ? "Use a CC number from 0 to 127." : "Use C#4 or a MIDI pitch from 0 to 127.");
            return;
        }
        op["semitones"] = pitch - r.pitch;
    } else if (column == Column::Value) {
        if (!integer(text, value) || value < (r.cc ? 0 : 1) || value > 127) {
            bad(r.cc ? "Use a value from 0 to 127." : "Use a velocity from 1 to 127.");
            return;
        }
        op["op"] = "velocity";
        op["velocity"] = value;
    }
    if (r.cc) {
        auto clear = json::Value::object();
        clear["op"] = "cc.clear";
        clear["number"] = r.pitch;
        clear["channel"] = r.channel;
        clear["from"] = r.beat - frameShift_;
        clear["to"] = r.beat - frameShift_ + tickBeat_ * 0.75;
        ops.push_back(std::move(clear));
        op = json::Value::object();
        op["op"] = "cc.set";
        op["number"] = pitch;
        op["channel"] = r.channel;
        auto point = json::Value::object();
        point["beat"] = beat - frameShift_;
        point["value"] = value;
        auto points = json::Value::array();
        points.push_back(std::move(point));
        op["points"] = std::move(points);
    }
    ops.push_back(std::move(op));
    // Build the operations before hiding the field; a command refreshes the rows.
    cancel();
    if (auto result = editor_.edit(std::move(ops)); !result)
        status(result.error().message);
    else {
        status("");
        sync();
        if (r.cc)
            for (const auto& row : rows_)
                if (row.cc && row.pitch == pitch && row.channel == r.channel &&
                    std::abs(row.beat - beat) < tickBeat_ * 0.5)
                    selectedCc_.insert(row.index);
        refreshSelection();
    }
    root_.focus(list_);
}

void MidiEventList::removeSelected() {
    sync();
    auto ops = json::Value::array();
    auto notes = json::Value::array();
    for (const auto& row : rows_)
        if (selected(static_cast<std::size_t>(&row - rows_.data()))) {
            if (!row.cc)
                notes.push_back(static_cast<std::int64_t>(row.index));
            else {
                auto op = json::Value::object();
                op["op"] = "cc.clear";
                op["number"] = row.pitch;
                op["channel"] = row.channel;
                op["from"] = row.beat - frameShift_;
                op["to"] = row.beat - frameShift_ + tickBeat_ * 0.75;
                ops.push_back(std::move(op));
            }
        }
    if (!notes.asArray().empty()) {
        auto op = json::Value::object();
        op["op"] = "remove";
        op["notes"] = std::move(notes);
        ops.push_back(std::move(op));
    }
    if (ops.asArray().empty()) return;
    if (auto result = editor_.edit(std::move(ops)); !result)
        status(result.error().message);
    else {
        status("");
        sync();
        refreshSelection();
    }
}

void MidiEventList::add() {
    sync();
    double beat = 0;
    if (const auto row = list_->selected(); row && *row < rows_.size()) beat = rows_[*row].beat;
    auto op = json::Value::object();
    op["op"] = "add";
    op["pitch"] = 60;
    op["velocity"] = 96;
    op["start"] = beat - frameShift_;
    op["length"] = 1.0;
    auto ops = json::Value::array();
    ops.push_back(std::move(op));
    if (auto result = editor_.edit(std::move(ops)); !result)
        status(result.error().message);
    else {
        status("");
        sync();
        refreshSelection();
    }
    root_.focus(list_);
}
}  // namespace supple::gui
