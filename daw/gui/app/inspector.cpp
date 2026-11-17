// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "inspector.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include "supple/ui/controls.h"
#include "supple/ui/forms.h"
#include "supple/ui/list.h"
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

// The track colours as swatches, then "none": a click picks one. The colour
// every selected track has is ringed; tracks of several colours ring none.
class ColorRow : public ui::Node {
  public:
    explicit ColorRow(std::string id) : Node(std::move(id), ui::Role::Button) {
        interactive = true;
        layout.height = ui::SizeRule::fixed(kSize);
    }
    static constexpr float kSize = 18, kGap = 4;
    std::function<void(const std::string&)> pick;
    std::optional<std::string> current;  // "" is none; nothing is mixed
    static ui::Rect swatch(const ui::Rect& b, std::size_t i) {
        return {b.x + static_cast<float>(i) * (kSize + kGap), b.y, kSize, kSize};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        for (std::size_t i = 0; i <= model::kTrackColors.size(); ++i) {
            const ui::Rect r = swatch(bounds(), i);
            const bool none = i == model::kTrackColors.size();
            const std::string name = none ? std::string() : std::string(model::kTrackColors[i]);
            if (none)  // an empty square: no colour
                c.strokeRound(r.inset(1), t.radius("s"), t.color("ink-faint"), 1);
            else if (const auto colour = t.trackColor(name))
                c.fillRound(r.inset(1), t.radius("s"), *colour);
            if (current && *current == name) c.strokeRound(r, t.radius("s"), t.color("ink"), 2);
        }
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Down || e.button != 1)
            return e.type == ui::PointerEvent::Type::Up;
        for (std::size_t i = 0; i <= model::kTrackColors.size(); ++i)
            if (swatch(bounds(), i).contains(e.pos)) {
                if (pick)
                    pick(i == model::kTrackColors.size() ? std::string()
                                                         : std::string(model::kTrackColors[i]));
                return true;
            }
        return true;
    }
    std::string valueText() const override {
        return !current ? "mixed" : current->empty() ? "none" : *current;
    }
};

// The value every one of `things` has, or nothing when they differ.
template <class T, class F>
auto common(const std::vector<const T*>& things, F get)
    -> std::optional<decltype(get(*things[0]))> {
    std::optional<decltype(get(*things[0]))> value;
    for (const T* thing : things) {
        auto one = get(*thing);
        if (value && !(*value == one)) return std::nullopt;
        value = std::move(one);
    }
    return value;
}

// What an edit is called in the history.
std::string editLabel(bool tracks, const std::string& key) {
    static const std::map<std::string, const char*> names{{"name", "name"},
                                                          {"position", "start"},
                                                          {"length", "length"},
                                                          {"timebase", "timebase"},
                                                          {"gain_db", "gain"},
                                                          {"fade_in", "fade in"},
                                                          {"fade_out", "fade out"},
                                                          {"mute", "mute"},
                                                          {"loop", "loop"},
                                                          {"locked", "lock"},
                                                          {"color", "color"},
                                                          {"volume_db", "volume"},
                                                          {"pan", "pan"}};
    const auto found = names.find(key);
    return std::string(tracks ? "Track " : "Item ") +
           (found == names.end() ? key.c_str() : found->second);
}

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
        out.ids = {id};
        out.title = model::trackLabel(s, id);
        out.detail = stripWord(track->type);
        out.color = theme.trackColor(track->color);
    } else if (showTracks) {
        out.kind = InspectorSubject::Kind::Tracks;
        for (const auto& [id, track] : tracks) out.ids.push_back(id);
        out.title = std::to_string(tracks.size()) + " tracks";
        out.detail = "Selected together";
    } else if (items.size() == 1) {
        const auto& [id, item] = items.front();
        const model::Track* track = s.tracks.find(item->track);
        out.kind = InspectorSubject::Kind::Item;
        out.ids = {id};
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
        for (const auto& [id, item] : items) out.ids.push_back(id);
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
    ui::Node& row = formRow(parent, key, label);
    rows_[key] = &row;
    auto& f = row.emplace<ui::TextField>("inspector." + key);
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
    rows_["timebase"] = &follows;
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

    // The heard take, when it plays audio: each one take.set.
    auto& take = box.emplace<ui::Node>("inspector.take");
    takeBox_ = &take;
    take.layout.dir = ui::Layout::Dir::Column;
    take.layout.height = ui::SizeRule::content();
    take.layout.gap = 4;
    field(take, "playrate", "Speed");
    field(take, "pitch", "Pitch");
    auto& stretch = formRow(take, "stretch", "Stretch");
    keepPitch_ = &stretch.emplace<ui::Toggle>("inspector.preserve_pitch", "Keep pitch", "accent",
                                              [this](bool on) { setTake("preserve_pitch", on); });
    keepPitch_->pill = true;
    keepPitch_->layout.width = ui::SizeRule::content();

    // Its takes, when it has several: a click chooses the one heard.
    auto& takes = box.emplace<ui::Node>("inspector.takes.box");
    takesBox_ = &takes;
    takes.layout.dir = ui::Layout::Dir::Column;
    takes.layout.height = ui::SizeRule::content();
    takes.layout.gap = 2;
    takes.emplace<ui::Label>("inspector.takes.title", "Takes", ui::Font::Ui, "small", "ink-quiet")
        .layout.height = ui::SizeRule::fixed(20);
    takes_ = &takes.emplace<ui::List>("inspector.takes");
    takes_->rowHeight = 22;
    takes_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
        const ui::Theme& t = c.theme();
        const bool heard = i == heard_;
        if (heard) {
            c.fillRound(r.inset(1), t.radius("s"), t.color("selection"));
            c.fillRound({r.x + 8, r.y + r.h / 2 - 3, 6, 6}, 3, t.color("accent"));
        }
        c.text({r.x + 22, r.y, r.w - 30, r.h}, takeNames_[i],
               heard ? ui::Font::UiBold : ui::Font::Ui, t.type("small"),
               t.color(heard ? "ink" : "ink-quiet"));
    };
    takes_->onSelect = [this](std::size_t i) {
        if (kind_ != InspectorSubject::Kind::Item || i >= takeIds_.size() || i == heard_) return;
        json::Value args = json::Value::object();
        args["item"] = ids_.front().str();
        args["take"] = takeIds_[i].str();
        run("take.activate", std::move(args));
    };
    file_ =
        &box.emplace<ui::TextBlock>("inspector.file", "", ui::Font::Mono, "caption", "ink-quiet");
    file_->layout.height = ui::SizeRule::content();

    // Tracks: each field one track.set on every selected track.
    auto& tracks = panel->emplace<ui::Node>("inspector.track");
    trackBox_ = &tracks;
    tracks.layout.dir = ui::Layout::Dir::Column;
    tracks.layout.height = ui::SizeRule::content();
    tracks.layout.gap = 4;
    tracks.visible = false;
    field(tracks, "track.name", "Name");
    auto& colour = formRow(tracks, "track.color", "Colour");
    auto& swatches = colour.emplace<ColorRow>("inspector.track.color");
    swatches.layout.width = ui::SizeRule::fraction();
    swatches.pick = [this](const std::string& name) { set("color", name); };
    colors_ = &swatches;
    field(tracks, "track.volume_db", "Volume");
    field(tracks, "track.pan", "Pan");
    return panel;
}

void Inspector::setVisible(bool visible) {
    visible_ = visible;
    if (visible) {
        version_ = ~std::uint64_t{0};  // read afresh
        sync();
    }
}

void Inspector::show(const std::string& key, std::optional<std::string> text, bool force) {
    Field& f = fields_[key];
    f.shown = text.value_or(std::string());
    // Several values: an empty field that says so.
    if (!text) f.node->setPlaceholder("mixed");
    // What is being typed stays, unless the field now shows something else
    // or the edit has just been applied or refused.
    if (!force && root_.focused() == f.node) return;
    if (f.node->text() != f.shown) f.node->setText(f.shown);
}

void Inspector::read(bool force) {
    using Kind = InspectorSubject::Kind;
    if (kind_ == Kind::Item || kind_ == Kind::Items)
        readItems(force);
    else if (kind_ == Kind::Track || kind_ == Kind::Tracks)
        readTracks(force);
}

void Inspector::readItems(bool force) {
    const model::Session& s = app_.head();
    std::vector<const model::Item*> items;
    for (const model::Id& id : ids_)
        if (const model::Item* item = s.items.find(id)) items.push_back(item);
    if (items.empty()) return;
    const bool one = items.size() == 1;
    const model::Item* item = items.front();
    const model::Take* take = one && item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
    // One item: its name and place. Several: only what they can share.
    for (const char* key : {"name", "start", "length"}) rows_[key]->visible = one;
    if (one) {
        show("name", take ? take->name : std::string(), force);
        // Unnamed, an item goes by its file's name (the arrange view's label).
        const std::string fallback = take ? views::Lanes::itemLabel(s, *item) : std::string();
        fields_["name"].node->setPlaceholder(!take                 ? "no take to name"
                                             : !take->name.empty() ? std::string()
                                                                   : fallback);
        show("start", inspectorBarsText(model::itemSpan(s, *item).startBeat, *s.timeMap), force);
        show("length", plain(model::itemLengthBeats(s, *item), 3) + " beats", force);
    }
    auto text = [&](auto get, int decimals, const char* unit) -> std::optional<std::string> {
        const auto value = common(items, get);
        return value ? std::optional<std::string>(fixed(*value, decimals, unit)) : std::nullopt;
    };
    show("gain_db", text([](const model::Item& i) { return i.gainDb; }, 1, "dB"), force);
    show("fade_in", text([](const model::Item& i) { return i.fadeIn.length; }, 3, "s"), force);
    show("fade_out", text([](const model::Item& i) { return i.fadeOut.length; }, 3, "s"), force);
    const auto timebase = common(items, [](const model::Item& i) { return i.timebase; });
    timebase_->setValue(timebase ? timebaseName(*timebase) : "");
    // A pill is lit when every item has the state on.
    auto all = [&](bool model::Item::* flag) {
        return std::all_of(items.begin(), items.end(),
                           [&](const model::Item* i) { return i->*flag; });
    };
    states_["mute"]->setOn(all(&model::Item::mute));
    states_["loop"]->setOn(all(&model::Item::loop));
    states_["locked"]->setOn(all(&model::Item::locked));
    // A locked item refuses every edit but its unlocking (item.set), and an
    // edit of several is all or none.
    const bool locked =
        std::any_of(items.begin(), items.end(), [](const model::Item* i) { return i->locked; });
    for (auto& [key, f] : fields_)
        if (key.rfind("track.", 0) != 0) f.node->enabled = !locked && (key != "name" || take);
    timebase_->enabled = !locked;
    states_["mute"]->enabled = states_["loop"]->enabled = !locked;
    const std::string note = !locked ? std::string()
                             : one   ? "Locked. Turn Lock off to edit."
                                     : "Some of these are locked. Turn Lock off to edit.";
    if (locked_->text() != note) locked_->setText(note);
    locked_->visible = locked;
    const model::MediaSource* media = take && take->source ? s.media.find(*take->source) : nullptr;
    takeBox_->visible = media != nullptr;
    if (media) {
        show("playrate", fixed(take->playrate, 3, "x"), force);
        show("pitch", fixed(take->pitch, 2, "st"), force);
        keepPitch_->setOn(take->preservePitch);
        keepPitch_->enabled = !locked;
    }
    // The take list, in the item's order; an unnamed take goes by its place.
    takeIds_ = one ? item->takes : std::vector<model::Id>{};
    takeNames_.clear();
    heard_ = takeIds_.size();
    for (std::size_t i = 0; i < takeIds_.size(); ++i) {
        const model::Take* tk = s.takes.find(takeIds_[i]);
        takeNames_.push_back(tk && !tk->name.empty() ? tk->name : "Take " + std::to_string(i + 1));
        if (item->activeTake && takeIds_[i] == *item->activeTake) heard_ = i;
    }
    takesBox_->visible = takeIds_.size() > 1;
    takes_->setCount(takeIds_.size());
    takes_->select(heard_ < takeIds_.size() ? std::optional<std::size_t>(heard_) : std::nullopt);
    takes_->layout.height =
        ui::SizeRule::fixed(22.0f * static_cast<float>(std::min<std::size_t>(takeIds_.size(), 6)));
    takes_->enabled = !locked;
    takes_->invalidate();
    const std::string file = media ? fileText(*media) : std::string();
    if (file_->text() != file) file_->setText(file);
    file_->visible = !file.empty();
}

void Inspector::readTracks(bool force) {
    const model::Session& s = app_.head();
    std::vector<const model::Track*> tracks;
    for (const model::Id& id : ids_)
        if (const model::Track* track = s.tracks.find(id)) tracks.push_back(track);
    if (tracks.empty()) return;
    const bool one = tracks.size() == 1;
    rows_["track.name"]->visible = one;
    // The name as stored: an unnamed track shows its label as a placeholder.
    if (one) {
        show("track.name", tracks.front()->name, force);
        fields_["track.name"].node->setPlaceholder(
            tracks.front()->name.empty() ? model::trackLabel(s, ids_.front()) : std::string());
    }
    const auto volume = common(tracks, [](const model::Track& t) { return t.volumeDb; });
    const auto pan = common(tracks, [](const model::Track& t) { return t.pan; });
    show("track.volume_db",
         volume ? std::optional<std::string>(fixed(*volume, 1, "dB")) : std::nullopt, force);
    show("track.pan", pan ? std::optional<std::string>(plain(*pan, 1) + " %") : std::nullopt,
         force);
    for (const char* key : {"track.name", "track.volume_db", "track.pan"})
        fields_[key].node->enabled = true;
    auto* row = static_cast<ColorRow*>(colors_);
    row->current = common(tracks, [](const model::Track& t) { return t.color; });
    row->invalidate();
}

void Inspector::fail(const std::string& message) {
    if (actions_.onError) actions_.onError(message);
    read(true);
    root_.relayout();
    panel_->invalidate();
}

void Inspector::finish(const Result<json::Value>& result) {
    if (!result) {
        fail(result.error().hint.empty() ? result.error().message
                                         : result.error().message + ": " + result.error().hint);
        return;
    }
    // The fields now; the header (a new name) at the next pass.
    version_ = ~std::uint64_t{0};
    read(true);
    root_.relayout();
    panel_->invalidate();
}

void Inspector::run(const char* command, json::Value args) {
    finish(dispatcher_.execute({command, std::move(args), kOrigin}));
}

void Inspector::set(const std::string& key, json::Value value) {
    using Kind = InspectorSubject::Kind;
    if (ids_.empty() || kind_ == Kind::Nothing) return;
    const bool tracks = kind_ == Kind::Track || kind_ == Kind::Tracks;
    const char* command = tracks ? "track.set" : "item.set";
    auto one = [&](const model::Id& id) {
        json::Value args = json::Value::object();
        args[tracks ? "track" : "item"] = id.str();
        args[key] = value;
        return dispatcher_.execute({command, std::move(args), kOrigin});
    };
    if (ids_.size() == 1) {
        finish(one(ids_.front()));
        return;
    }
    // Several: all of them or none, as one undo step.
    const std::vector<model::Id> ids = ids_;
    finish(dispatcher_.transact(editLabel(tracks, key), kOrigin, [&]() -> Result<json::Value> {
        for (const model::Id& id : ids) {
            const model::Session& s = app_.head();
            if (tracks ? s.tracks.find(id) == nullptr : s.items.find(id) == nullptr)
                continue;  // gone meanwhile
            // A locked item refuses every edit but its unlocking, locking it
            // again included: Lock on a selection locks the ones that are not.
            if (!tracks && key == "locked" && value.isBool() && value.asBool() &&
                s.items.find(id)->locked)
                continue;
            auto r = one(id);
            if (!r) return r;
        }
        return json::Value::object();
    }));
}

void Inspector::setTake(const std::string& key, json::Value value) {
    if (kind_ != InspectorSubject::Kind::Item) return;
    const model::Item* item = app_.head().items.find(ids_.front());
    if (!item || !item->activeTake) return;
    json::Value args = json::Value::object();
    args["take"] = item->activeTake->str();
    args[key] = std::move(value);
    run("take.set", std::move(args));
}

void Inspector::commit(const std::string& key, const std::string& text) {
    if (ids_.empty() || text == fields_[key].shown) return;
    if (key == "name" || key == "track.name") {
        set("name", text);
        return;
    }
    const model::Session& s = app_.head();
    Result<double> value = key == "start"             ? inspectorParseBars(text, *s.timeMap)
                           : key == "length"          ? inspectorParseNumber(text, "beats")
                           : key == "gain_db"         ? inspectorParseNumber(text, "dB")
                           : key == "track.volume_db" ? inspectorParseNumber(text, "dB")
                           : key == "track.pan"       ? inspectorParseNumber(text, "%")
                           : key == "playrate"        ? inspectorParseNumber(text, "x")
                           : key == "pitch"           ? inspectorParseNumber(text, "st")
                                                      : inspectorParseNumber(text, "s");
    if (!value) {
        fail(value.error().message);
        return;
    }
    if (key == "playrate" || key == "pitch")
        setTake(key, *value);
    else if (key == "start")
        set("position", *value);
    else
        set(key.rfind("track.", 0) == 0 ? key.substr(6) : key, *value);
}

void Inspector::sync() {
    if (!visible_ || !panel_) return;
    if (app_.head().version == version_ && app_.projectGeneration() == generation_ &&
        selection_.selectedTracks == tracks_ && selection_.selectedItems == items_ &&
        selection_.tracksFocused == tracksFocused_)
        return;
    using Kind = InspectorSubject::Kind;
    InspectorSubject subject = inspectorSubject(app_.head(), selection_, root_.theme());
    const bool another = subject.kind != kind_ || subject.ids != ids_;
    if (another && !ids_.empty()) {
        // Leaving a field applies it, to what it showed: that edit makes a
        // new head, so the subject is read again after it.
        root_.releaseWithin(itemBox_);
        root_.releaseWithin(trackBox_);
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
    kind_ = subject.kind;
    ids_ = subject.ids;
    itemBox_->visible = kind_ == Kind::Item || kind_ == Kind::Items;
    trackBox_->visible = kind_ == Kind::Track || kind_ == Kind::Tracks;
    read(another);
    root_.relayout();
    panel_->invalidate();
}

}  // namespace supple::gui
