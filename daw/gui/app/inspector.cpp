// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "inspector.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

#include "supple/ui/controls.h"
#include "supple/ui/forms.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "supple/views/lanes.h"

namespace supple::gui {
namespace {

// The bar beside the title, in the selected thing's colour.
class Swatch : public ui::Node {
  public:
    using Node::Node;
    std::optional<ui::Color> color;
    void paint(ui::Canvas& c) override {
        if (color) c.fillRound(bounds(), 2, *color);
    }
};

const char* stripWord(model::StripType type) {
    switch (type) {
        case model::StripType::Channel:
            return "Channel";
        case model::StripType::Buss:
            return "Buss";
        case model::StripType::Master:
            return "Master";
    }
    return "Track";
}

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

// The whole of `text` as a number, or nothing.
std::optional<double> number(std::string_view text) {
    text = trimmed(text);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    double value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return {};
    return value;
}

// "32", "0.5", "1.333": at most `decimals`, no trailing zeros.
std::string plain(double value, int decimals) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, value);
    std::string text(buf);
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text == "-0" ? "0" : text;
}

std::string fixed(double value, int decimals, const char* unit) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f %s", decimals, value, unit);
    return buf;
}

const char* timebaseName(model::ItemTimebase timebase) {
    switch (timebase) {
        case model::ItemTimebase::Track:
            return "track";
        case model::ItemTimebase::Time:
            return "time";
        case model::ItemTimebase::Beats:
            return "beats";
        case model::ItemTimebase::BeatsPosition:
            return "beats_position";
    }
    return "track";
}

// What an audio item plays, for the File line.
std::string fileText(const model::MediaSource& media) {
    std::string text = media.path;
    std::string facts;
    if (media.sampleRate > 0) facts = plain(media.sampleRate / 1000.0, 1) + " kHz";
    if (media.channels > 0)
        facts += (facts.empty() ? "" : ", ") + (media.channels == 1 ? std::string("mono")
                                                : media.channels == 2
                                                    ? std::string("stereo")
                                                    : std::to_string(media.channels) + " channels");
    if (media.sampleRate > 0 && media.frames > 0) {
        const auto seconds = std::llround(static_cast<double>(media.frames) / media.sampleRate);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld:%02lld", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
        facts += (facts.empty() ? "" : ", ") + std::string(buf);
    }
    return facts.empty() ? text : text + "\n" + facts;
}

template <class T>
std::vector<std::pair<model::Id, const T*>> held(const model::IdMap<T>& table,
                                                 const std::vector<std::string>& ids) {
    std::vector<std::pair<model::Id, const T*>> out;
    for (const std::string& text : ids)
        if (const auto id = model::Id::parse(text))
            if (const T* value = table.find(*id)) out.emplace_back(*id, value);
    return out;
}

}  // namespace

InspectorSubject inspectorSubject(const model::Session& s, const UiState& selection,
                                  const ui::Theme& theme) {
    InspectorSubject out;
    const auto tracks = held(s.tracks, selection.selectedTracks);
    const auto items = held(s.items, selection.selectedItems);
    const bool showTracks = !tracks.empty() && (selection.tracksFocused || items.empty());
    if (showTracks && tracks.size() == 1) {
        const auto& [id, track] = tracks.front();
        out.kind = InspectorSubject::Kind::Track;
        out.title = model::trackLabel(s, id);
        out.detail = stripWord(track->type);
        out.color = theme.trackColor(track->color);
    } else if (showTracks) {
        out.kind = InspectorSubject::Kind::Tracks;
        out.title = std::to_string(tracks.size()) + " tracks";
        out.detail = "Selected together";
    } else if (items.size() == 1) {
        const auto& [id, item] = items.front();
        const model::Track* track = s.tracks.find(item->track);
        out.kind = InspectorSubject::Kind::Item;
        out.item = id;
        out.title = views::Lanes::itemLabel(s, *item);
        if (out.title.empty()) out.title = id.str();
        const model::Take* take = item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
        const char* what = !take || !take->source                   ? "Empty item"
                           : s.media.find(*take->source) != nullptr ? "Audio item"
                                                                    : "MIDI item";
        out.detail = std::string(what) + " on " + model::trackLabel(s, item->track);
        out.color = views::Lanes::itemColor(theme, *item, track,
                                            track ? track->type : model::StripType::Channel);
    } else if (!items.empty()) {
        out.kind = InspectorSubject::Kind::Items;
        out.title = std::to_string(items.size()) + " items";
        std::set<model::Id> on;
        for (const auto& [id, item] : items) on.insert(item->track);
        out.detail = on.size() == 1 ? "On " + model::trackLabel(s, *on.begin())
                                    : "On " + std::to_string(on.size()) + " tracks";
    } else {
        out.title = "Nothing selected";
        out.detail = "Click an item or a track to see it here.";
    }
    return out;
}

std::string inspectorBarsText(double beat, const time::TimeMap& map) {
    const time::BarBeat at = map.barBeat(beat);
    // To the nearest thousandth, never a fourth digit: .9996 stays .999.
    const long long parts = std::min(999LL, std::llround(at.fraction * 1000.0));
    char buf[48];
    std::snprintf(buf, sizeof buf, "%d.%d.%03lld", at.bar, at.beat, parts);
    return buf;
}

Result<double> inspectorParseBars(std::string_view text, const time::TimeMap& map) {
    const Error bad("gui.inspector",
                    "write a bar, or bar.beat, or bar.beat.thousandths: 3, 3.2 or 3.2.500");
    text = trimmed(text);
    long long part[3] = {1, 1, 0};
    int count = 0;
    while (true) {
        const auto dot = text.find('.');
        const std::string_view piece = text.substr(0, dot);
        if (count == 3 || piece.empty()) return bad;
        const auto [end, ec] =
            std::from_chars(piece.data(), piece.data() + piece.size(), part[count]);
        if (ec != std::errc{} || end != piece.data() + piece.size() || part[count] < 0) return bad;
        ++count;
        if (dot == std::string_view::npos) break;
        text.remove_prefix(dot + 1);
    }
    if (part[0] < 1 || part[0] > 1'000'000 || part[1] < 1 || part[2] > 999) return bad;
    const int bar = static_cast<int>(part[0]);
    const time::MeterChange meter = map.meterAt(bar);
    if (part[1] > meter.numerator)
        return Error("gui.inspector", "bar " + std::to_string(bar) + " has " +
                                          std::to_string(meter.numerator) + " beats");
    // A beat of the meter is 4/denominator quarter notes, the session's beat.
    const double unit = 4.0 / meter.denominator;
    return map.beatOfBar(bar) +
           (static_cast<double>(part[1] - 1) + static_cast<double>(part[2]) / 1000.0) * unit;
}

Result<double> inspectorParseNumber(std::string_view text, std::string_view unit) {
    text = trimmed(text);
    // The unit, if typed, in either case and with or without a space.
    if (text.size() >= unit.size() && !unit.empty()) {
        const std::string_view end = text.substr(text.size() - unit.size());
        bool same = true;
        for (std::size_t i = 0; i < unit.size(); ++i)
            same = same && std::tolower(static_cast<unsigned char>(end[i])) ==
                               std::tolower(static_cast<unsigned char>(unit[i]));
        if (same) text.remove_suffix(unit.size());
    }
    if (const auto value = number(text)) return *value;
    return Error("gui.inspector",
                 "write a number" +
                     (unit.empty() ? std::string() : ", for example 1.5 " + std::string(unit)));
}

Inspector::Inspector(ui::Root& root, engine::EngineApp& app, const UiState& selection,
                     cmd::Dispatcher& dispatcher, AppActions& actions)
    : root_(root), app_(app), selection_(selection), dispatcher_(dispatcher), actions_(actions) {}

namespace {

// A row of the form: its label at the left, the control filling the rest.
ui::Node& formRow(ui::Node& parent, const std::string& key, const std::string& label) {
    auto& row = parent.emplace<ui::Node>("inspector.row." + key);
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(24);
    row.layout.gap = 8;
    row.layout.align = ui::Layout::Align::Center;
    auto& name =
        row.emplace<ui::Label>("inspector.label." + key, label, ui::Font::Ui, "small", "ink-quiet");
    name.layout.width = ui::SizeRule::fixed(72);
    return row;
}

}  // namespace

ui::TextField& Inspector::field(ui::Node& parent, const std::string& key,
                                const std::string& label) {
    auto& f = formRow(parent, key, label).emplace<ui::TextField>("inspector." + key);
    f.layout.width = ui::SizeRule::fraction();
    f.layout.height = ui::SizeRule::fixed(22);
    f.onSubmit = [this, key](const std::string& text) { commit(key, text); };
    f.onBlur = [this, key](const std::string& text) { commit(key, text); };
    f.onCancel = [this, key] { fields_[key].node->setText(fields_[key].shown); };
    fields_[key].node = &f;
    return f;
}

ui::Toggle& Inspector::state(ui::Node& parent, const std::string& key, const std::string& label,
                             const std::string& color) {
    auto& t = parent.emplace<ui::Toggle>("inspector." + key, label, color,
                                         [this, key](bool on) { set(key, on); });
    t.pill = true;
    t.layout.width = ui::SizeRule::content();
    states_[key] = &t;
    return t;
}

std::unique_ptr<ui::Node> Inspector::build() {
    auto panel = std::make_unique<ui::Node>("inspector");
    panel_ = panel.get();
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.padding = ui::Insets::all(12);
    panel->layout.gap = 8;
    auto& header = panel->emplace<ui::Node>("inspector.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::content();
    header.layout.gap = 8;
    auto& swatch = header.emplace<Swatch>("inspector.swatch");
    swatch.layout.width = ui::SizeRule::fixed(6);
    swatch_ = &swatch;
    auto& text = header.emplace<ui::Node>("inspector.heading");
    text.layout.dir = ui::Layout::Dir::Column;
    text.layout.width = ui::SizeRule::fraction();
    text.layout.height = ui::SizeRule::content();
    text.layout.gap = 2;
    title_ = &text.emplace<ui::Label>("inspector.title", "", ui::Font::UiBold, "title");
    detail_ =
        &text.emplace<ui::TextBlock>("inspector.detail", "", ui::Font::Ui, "small", "ink-quiet");
    detail_->layout.height = ui::SizeRule::content();

    // One item: its fields, each one item.set (session-model.md).
    auto& box = panel->emplace<ui::Node>("inspector.item");
    itemBox_ = &box;
    box.layout.dir = ui::Layout::Dir::Column;
    box.layout.height = ui::SizeRule::content();
    box.layout.gap = 4;
    box.visible = false;
    locked_ = &box.emplace<ui::TextBlock>("inspector.locked.note", "Locked. Turn Lock off to edit.",
                                          ui::Font::Ui, "small", "ink-quiet");
    locked_->layout.height = ui::SizeRule::content();
    field(box, "name", "Name");
    field(box, "start", "Start");
    field(box, "length", "Length");
    auto& follows = formRow(box, "timebase", "Follows");
    timebase_ = &follows.emplace<ui::Dropdown>(
        "inspector.timebase", std::vector<ui::Choice>{{"track", "Its track's setting"},
                                                      {"beats", "Beats"},
                                                      {"time", "Time"},
                                                      {"beats_position", "Beats, start only"}});
    timebase_->layout.width = ui::SizeRule::fraction();
    timebase_->layout.height = ui::SizeRule::fixed(22);
    timebase_->onChange = [this](const std::string& value) { set("timebase", value); };
    field(box, "gain_db", "Gain");
    field(box, "fade_in", "Fade in");
    field(box, "fade_out", "Fade out");
    auto& states = formRow(box, "state", "State");
    state(states, "mute", "Mute", "mute");
    state(states, "loop", "Loop", "accent");
    state(states, "locked", "Lock", "accent");
    file_ =
        &box.emplace<ui::TextBlock>("inspector.file", "", ui::Font::Mono, "caption", "ink-quiet");
    file_->layout.height = ui::SizeRule::content();
    return panel;
}

void Inspector::setVisible(bool visible) {
    visible_ = visible;
    if (visible) {
        version_ = ~std::uint64_t{0};  // read afresh
        sync();
    }
}

void Inspector::show(const std::string& key, std::string text, bool force) {
    Field& f = fields_[key];
    f.shown = text;
    // What is being typed stays, unless the field now shows another item or
    // the edit has just been applied or refused.
    if (!force && root_.focused() == f.node) return;
    if (f.node->text() != text) f.node->setText(std::move(text));
}

void Inspector::readItem(bool force) {
    const model::Session& s = app_.head();
    const model::Item* item = item_ ? s.items.find(*item_) : nullptr;
    if (!item) return;
    const model::Take* take = item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
    show("name", take ? take->name : std::string(), force);
    // Unnamed, an item goes by its file's name (the arrange view's label).
    const std::string fallback = take ? views::Lanes::itemLabel(s, *item) : std::string();
    fields_["name"].node->setPlaceholder(!take                 ? "no take to name"
                                         : !take->name.empty() ? std::string()
                                                               : fallback);
    show("start", inspectorBarsText(model::itemSpan(s, *item).startBeat, *s.timeMap), force);
    show("length", plain(model::itemLengthBeats(s, *item), 3) + " beats", force);
    show("gain_db", fixed(item->gainDb, 1, "dB"), force);
    show("fade_in", fixed(item->fadeIn.length, 3, "s"), force);
    show("fade_out", fixed(item->fadeOut.length, 3, "s"), force);
    timebase_->setValue(timebaseName(item->timebase));
    states_["mute"]->setOn(item->mute);
    states_["loop"]->setOn(item->loop);
    states_["locked"]->setOn(item->locked);
    // A locked item refuses every edit but its unlocking (item.set).
    for (auto& [key, f] : fields_) f.node->enabled = !item->locked && (key != "name" || take);
    timebase_->enabled = !item->locked;
    states_["mute"]->enabled = states_["loop"]->enabled = !item->locked;
    locked_->visible = item->locked;
    const model::MediaSource* media = take && take->source ? s.media.find(*take->source) : nullptr;
    const std::string file = media ? fileText(*media) : std::string();
    if (file_->text() != file) file_->setText(file);
    file_->visible = !file.empty();
}

void Inspector::fail(const std::string& message) {
    if (actions_.onError) actions_.onError(message);
    readItem(true);
    root_.relayout();
    panel_->invalidate();
}

void Inspector::set(const std::string& key, json::Value value) {
    if (!item_) return;
    json::Value args = json::Value::object();
    args["item"] = item_->str();
    args[key] = std::move(value);
    const auto r = dispatcher_.execute({"item.set", std::move(args), kOrigin});
    if (!r) {
        fail(r.error().hint.empty() ? r.error().message
                                    : r.error().message + ": " + r.error().hint);
        return;
    }
    // The fields now; the header (a new name) at the next pass.
    version_ = ~std::uint64_t{0};
    readItem(true);
    root_.relayout();
    panel_->invalidate();
}

void Inspector::commit(const std::string& key, const std::string& text) {
    if (!item_ || text == fields_[key].shown) return;
    if (key == "name") {
        set(key, text);
        return;
    }
    const model::Session& s = app_.head();
    Result<double> value = key == "start"     ? inspectorParseBars(text, *s.timeMap)
                           : key == "length"  ? inspectorParseNumber(text, "beats")
                           : key == "gain_db" ? inspectorParseNumber(text, "dB")
                                              : inspectorParseNumber(text, "s");
    if (!value) {
        fail(value.error().message);
        return;
    }
    set(key == "start" ? "position" : key, *value);
}

void Inspector::sync() {
    if (!visible_ || !panel_) return;
    if (app_.head().version == version_ && app_.projectGeneration() == generation_ &&
        selection_.selectedTracks == tracks_ && selection_.selectedItems == items_ &&
        selection_.tracksFocused == tracksFocused_)
        return;
    InspectorSubject subject = inspectorSubject(app_.head(), selection_, root_.theme());
    const bool another = subject.item != item_;
    if (another && item_) {
        // Leaving a field applies it, to the item it showed: that edit makes
        // a new head, so the subject is read again after it.
        root_.releaseWithin(itemBox_);
        subject = inspectorSubject(app_.head(), selection_, root_.theme());
    }
    version_ = app_.head().version;
    generation_ = app_.projectGeneration();
    tracks_ = selection_.selectedTracks;
    items_ = selection_.selectedItems;
    tracksFocused_ = selection_.tracksFocused;
    if (title_->text() != subject.title) title_->setText(subject.title);
    if (detail_->text() != subject.detail) detail_->setText(subject.detail);
    static_cast<Swatch*>(swatch_)->color = subject.color;
    swatch_->visible = subject.color.has_value();
    item_ = subject.item;
    itemBox_->visible = item_.has_value();
    if (item_) readItem(another);
    root_.relayout();
    panel_->invalidate();
}

}  // namespace supple::gui
