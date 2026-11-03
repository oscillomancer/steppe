// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "render_dialog.h"
#include <supple/base/paths.h>

#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <tuple>

#include "supple/json/write.h"
#include "supple/ui/controls.h"
#include "supple/ui/forms.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};
using ui::Choice;

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::optional<double> number(std::string_view text) {
    if (text.empty()) return {};
    const std::string s(text);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return {};
    return v;
}

std::optional<std::int64_t> integer(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

std::string baseName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

std::string text(const json::Value* v, std::string fallback = {}) {
    return v && v->isString() ? v->asString() : fallback;
}
double num(const json::Value* v, double fallback = 0) {
    return v && v->isNumber() ? v->asDouble() : fallback;
}
bool flag(const json::Value* v, bool fallback = false) {
    return v && v->isBool() ? v->asBool() : fallback;
}
std::string compact(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
// "−14" with a real minus sign, as the dialog shows numbers.
std::string signedText(double v) {
    std::string s = compact(v);
    if (!s.empty() && s[0] == '-') s = "−" + s.substr(1);
    return s;
}

const std::vector<Choice>& formatChoices() {
    static const std::vector<Choice> c{{"wav", "WAV"},
                                       {"aiff", "AIFF"},
                                       {"flac", "FLAC"},
                                       {"mp3", "MP3"},
                                       {"vorbis", "Ogg Vorbis"}};
    return c;
}
std::vector<Choice> depthChoices(const std::string& format) {
    if (format == "wav") return {{"16", "16-bit"}, {"24", "24-bit"}, {"32", "32-bit float"}};
    if (format == "aiff" || format == "flac") return {{"16", "16-bit"}, {"24", "24-bit"}};
    return {};
}
std::string formatLabel(const std::string& format) {
    for (const auto& c : formatChoices())
        if (c.value == format) return c.label;
    return format;
}
std::string sourceKey(RenderDialog::Source s) {
    return s == RenderDialog::Source::Master  ? "mix"
           : s == RenderDialog::Source::Stems ? "stems"
                                              : "regions";
}

ui::Node& row(ui::Node& parent, const std::string& id, float height = 26) {
    auto& r = parent.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(height);
    r.layout.gap = 6;
    r.layout.align = ui::Layout::Align::Center;
    return r;
}
ui::Label& caption(ui::Node& parent, const std::string& id, const std::string& label,
                   float width = 110) {
    auto& l = parent.emplace<ui::Label>(id, label, ui::Font::Ui, "caption", "ink-quiet");
    l.layout.width = ui::SizeRule::fixed(width);
    return l;
}
ui::Label& heading(ui::Node& parent, const std::string& id, const std::string& label) {
    auto& l = parent.emplace<ui::Label>(id, label, ui::Font::UiBold, "small", "ink");
    l.layout.height = ui::SizeRule::fixed(22);
    return l;
}
}  // namespace

std::string fileUrl(std::string_view path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "file://";
    std::size_t from = 0;
    const auto drive = [&](std::string_view p) {
        return p.size() >= 2 && p[1] == ':' &&
               ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
    };
    if (path.starts_with("//")) {  // UNC: the server is the URL's host
        from = 2;
    } else if (drive(path)) {
        out += '/';
        out += path.substr(0, 2);  // "C:"
        from = 2;
    }
    for (std::size_t i = from; i < path.size(); ++i) {
        const auto c = static_cast<unsigned char>(path[i]);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
                           c == '/';
        if (plain) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

RenderDialog::RenderDialog(engine::EngineApp& app, cmd::Dispatcher& dispatcher, ui::Root& root)
    : app_(app), dispatcher_(dispatcher), root_(root) {
    openFolder = [](const std::filesystem::path& dir) {
        const std::string url = fileUrl(dir.generic_string());
        (void)SDL_OpenURL(url.c_str());
    };
}

RenderDialog::~RenderDialog() = default;

// ------------------------------------------------------------------ state

void RenderDialog::load(const json::Value& args) {
    auto result = dispatcher_.execute({"render.settings", args, kUiOrigin});
    if (!result) {
        setStatus("render settings: " + result.error().message);
        return;
    }
    settings_ = *result->find("effective");
    from_ = *result->find("from");
    owned_.clear();
    const bool preset = args.isObject() && args.size() > 0;
    for (const auto& m : from_.asObject())
        if (m.value.isString() && m.value.asString() == (preset ? "args" : "project"))
            owned_.insert(m.key);
    if (!preset) {
        initial_ = settings_;
        initialOwned_ = owned_;
    }
    raw_.clear();
    errors_.clear();
    tab_ = 0;
}

json::Value& RenderDialog::member(const std::string& name) { return settings_[name]; }

json::Value& RenderDialog::output(std::size_t index) {
    return settings_["outputs"].asArray().at(index);
}

bool RenderDialog::fromDefault(const std::string& name) const {
    if (owned_.count(name)) return false;
    const auto* layer = from_.find(name);
    return layer && layer->isString() && layer->asString() == "default";
}

void RenderDialog::set(const std::string& name, json::Value value, bool structural) {
    if (value.isNull())
        settings_.asObject().erase(name);
    else
        settings_[name] = std::move(value);
    owned_.insert(name);
    markDirty();
    if (structural) rebuild();
}

json::Value RenderDialog::projectSettings() const {
    json::Value out = json::Value::object();
    for (const auto& name : owned_)
        if (const auto* v = settings_.find(name)) out[name] = *v;
    return out;
}

RenderDialog::Source RenderDialog::source() const {
    const std::string s = text(settings_.find("source"), "mix");
    return s == "stems" ? Source::Stems : s == "regions" ? Source::Regions : Source::Master;
}

void RenderDialog::setSource(Source s) { set("source", sourceKey(s), true); }

bool RenderDialog::addToProject() const { return flag(settings_.find("add_to_project")); }
void RenderDialog::setAddToProject(bool on) { set("add_to_project", on, true); }

std::string RenderDialog::pattern() const {
    const auto* p = settings_.find("pattern");
    if (!p) return {};
    if (p->isString()) return p->asString();
    return text(p->find(sourceKey(source())));
}

void RenderDialog::numberField(const std::string& id, const std::string& typed,
                               std::function<Status(double)> apply) {
    raw_[id] = typed;
    errors_.erase(id);
    markDirty();
    if (typed.empty()) {
        (void)apply(std::numeric_limits<double>::quiet_NaN());  // NaN: back to the default
        return;
    }
    const auto value = number(typed);
    if (!value) {
        errors_[id] = "enter a finite number";
        return;
    }
    if (auto applied = apply(*value); !applied) errors_[id] = applied.error().message;
}

void RenderDialog::setField(const std::string& id, const std::string& typed) {
    const auto range = [](double v, double lo, double hi) -> Status {
        if (v < lo || v > hi)
            return Error("gui.render.field", "use " + compact(lo) + " to " + compact(hi));
        return {};
    };
    const bool unset = typed.empty();
    if (id == "render.pattern") {
        json::Value p = settings_.find("pattern") ? *settings_.find("pattern") : json::Value();
        if (!p.isObject()) {
            const std::string all = text(&p);
            p = json::Value::object();
            for (const char* k : {"mix", "stems", "regions"}) p[k] = all;
        }
        p[sourceKey(source())] = typed;
        set("pattern", std::move(p));
        if (patternField_ && patternField_->text() != typed) patternField_->setText(typed);
        return;
    }
    if (id == "render.start" || id == "render.end") {
        numberField(id, typed, [&, id](double v) -> Status {
            const char* key = id == "render.start" ? "start_beat" : "end_beat";
            if (std::isnan(v)) {
                settings_.asObject().erase(key);
            } else {
                if (v < 0 || (id == "render.end" && v <= 0))
                    return Error("gui.render.field", "enter a positive number of beats");
                settings_[key] = v;
            }
            owned_.insert(key);
            const bool was = text(settings_.find("bounds")) == "custom";
            set("bounds", "custom", !was);  // the Range dropdown follows
            return {};
        });
    } else if (id == "render.tail") {
        numberField(id, typed, [&](double v) -> Status {
            if (!std::isnan(v)) SUPPLE_TRY(range(v, 0, 600));
            set("tail_s", std::isnan(v) ? 0.0 : v);
            return {};
        });
    } else if (id == "render.sample_rate") {
        raw_[id] = typed;
        errors_.erase(id);
        markDirty();
        if (unset) {
            set("sample_rate", static_cast<std::int64_t>(app_.head().project->sampleRate));
            owned_.erase("sample_rate");
        } else if (auto v = integer(typed); v && *v >= 8000 && *v <= 384000)
            set("sample_rate", *v);
        else
            errors_[id] = "use 8000..384000 Hz";
    } else if (id == "render.bit_depth") {
        raw_[id] = typed;
        errors_.erase(id);
        markDirty();
        if (auto v = integer(typed); v && (*v == 16 || *v == 24 || *v == 32)) {
            output(tab_)["bit_depth"] = *v;
            owned_.insert("outputs");
        } else if (!unset)
            errors_[id] = "use 16, 24 or 32 bits";
    } else if (id == "render.format") {
        if (unset) return;
        json::Value o = json::Value::object();
        o["format"] = typed;
        settings_["outputs"].asArray().at(tab_) = std::move(o);
        set("outputs", settings_["outputs"], true);
    } else if (id == "render.secondary") {
        auto& outputs = settings_["outputs"].asArray();
        outputs.resize(1);
        std::string spec = typed;
        while (!spec.empty() && spec.back() == ' ') spec.pop_back();
        if (!spec.empty() && spec != "none") {
            json::Value o = json::Value::object();
            const auto space = spec.find(' ');
            o["format"] = spec.substr(0, space);
            if (space != std::string::npos)
                if (auto v = integer(spec.substr(space + 1))) o["bit_depth"] = *v;
            outputs.push_back(std::move(o));
        }
        set("outputs", settings_["outputs"], true);
    } else if (id == "render.channels") {
        raw_[id] = typed;
        errors_.erase(id);
        markDirty();
        if (typed == "track")
            set("channels", "track");
        else if (auto v = integer(typed); v && (*v == 1 || *v == 2))
            set("channels", *v);
        else if (!unset)
            errors_[id] = "use one or two channels";
    } else if (id == "render.normalize") {
        json::Value n = settings_["normalize"];
        n["mode"] = unset ? "none" : typed;
        set("normalize", std::move(n), true);
    } else if (id == "render.target") {
        numberField(id, typed, [&](double v) -> Status {
            if (!std::isnan(v)) SUPPLE_TRY(range(v, -100, 0));
            json::Value n = settings_["normalize"];
            n["target"] = std::isnan(v) ? -14.0 : v;
            set("normalize", std::move(n));
            return {};
        });
    } else if (id == "render.limit") {
        numberField(id, typed, [&](double v) -> Status {
            if (!std::isnan(v)) SUPPLE_TRY(range(v, -60, 0));
            set("limit_true_peak_db", std::isnan(v) ? json::Value() : json::Value(v));
            return {};
        });
    } else if (id == "render.handles") {
        numberField(id, typed, [&](double v) -> Status {
            if (!std::isnan(v)) SUPPLE_TRY(range(v, 0, 60));
            set("handles_s", std::isnan(v) ? 0.0 : v);
            return {};
        });
    } else if (id == "render.threshold") {
        numberField(id, typed, [&](double v) -> Status {
            if (!std::isnan(v)) SUPPLE_TRY(range(v, -144, 0));
            json::Value t = settings_["trim"];
            t["threshold_db"] = std::isnan(v) ? -60.0 : v;
            set("trim", std::move(t));
            return {};
        });
    } else if (id == "render.vorbis_quality") {
        numberField(id, typed, [&](double v) -> Status {
            if (!std::isnan(v)) SUPPLE_TRY(range(v, -1, 10));
            output(tab_)["vorbis_quality"] = std::isnan(v) ? 6.0 : v;
            set("outputs", settings_["outputs"]);
            return {};
        });
    } else if (id == "render.output_rate") {
        raw_[id] = typed;
        errors_.erase(id);
        markDirty();
        if (unset)
            output(tab_).asObject().erase("sample_rate");
        else if (auto v = integer(typed); v && *v >= 8000 && *v <= 384000)
            output(tab_)["sample_rate"] = *v;
        else
            errors_[id] = "use 8000..384000 Hz";
        owned_.insert("outputs");
    } else if (id == "render.suffix") {
        if (unset)
            output(tab_).asObject().erase("suffix");
        else
            output(tab_)["suffix"] = typed;
        set("outputs", settings_["outputs"]);
    } else if (id == "render.mode" || id == "render.tap") {
        json::Value st = settings_["stems"];
        st[id == "render.mode" ? "mode" : "tap"] = typed;
        set("stems", std::move(st));
    } else if (id.starts_with("render.meta.")) {
        const std::string key = id.substr(std::string_view("render.meta.").size());
        json::Value m = settings_.find("metadata") ? settings_["metadata"] : json::Value::object();
        raw_[id] = typed;
        errors_.erase(id);
        if (unset)
            m.asObject().erase(key);
        else if (key == "track_number") {
            if (auto v = integer(typed); v && *v >= 1 && *v <= 9999)
                m[key] = *v;
            else
                errors_[id] = "use a track number from 1 to 9999";
        } else
            m[key] = typed;
        set("metadata", m.size() ? std::move(m) : json::Value());
    }
}

void RenderDialog::setCover(const std::string& relative) {
    json::Value m = settings_.find("metadata") ? settings_["metadata"] : json::Value::object();
    if (relative.empty())
        m.asObject().erase("cover");
    else
        m["cover"] = relative;
    set("metadata", m.size() ? std::move(m) : json::Value(), true);
}

Status RenderDialog::validate() const {
    if (errors_.empty()) return {};
    const auto& [field, message] = *errors_.begin();
    const std::string name = field.substr(field.find('.') + 1);
    return Error("gui.render.field", field + ": " + message, "/" + name,
                 "correct this field or clear it for the default");
}

json::Value RenderDialog::selectors() const {
    json::Value a = json::Value::object();
    if (versionChoice_.starts_with("branch:"))
        a["branch"] = versionChoice_.substr(7);
    else if (versionChoice_.starts_with("version:"))
        if (auto v = integer(versionChoice_.substr(8))) a["version"] = *v;
    if (regions_ && source() == Source::Regions) {
        json::Value ids = json::Value::array();
        for (const auto& id : *regions_) ids.push_back(id);
        a["regions"] = std::move(ids);
    }
    return a;
}

std::string RenderDialog::commandName() const {
    switch (source()) {
        case Source::Master:
            return "render.mix";
        case Source::Stems:
            return "render.stems";
        case Source::Regions:
            return "render.regions";
    }
    return "render.mix";
}

json::Value RenderDialog::renderArgs(bool forJob) const {
    json::Value a = settings_;
    const json::Value chosen = selectors();
    for (const auto& m : chosen.asObject()) a[m.key] = m.value;
    // A mix carries no per-file analysis unless it is asked for; the dialog
    // lists its loudness.
    if (source() == Source::Master) a["analyze"] = "summary";
    if (forJob) a["job"] = true;
    return a;
}

void RenderDialog::markDirty() {
    dirty_ = true;
    lastEditMs_ = nowMs();
}

void RenderDialog::setStatus(std::string message) {
    statusText_ = std::move(message);
    if (status_) status_->setText(statusText_);
}

Status RenderDialog::writeSettings() {
    bool changed = owned_ != initialOwned_;
    for (const auto& name : owned_) {
        if (changed) break;
        const auto* now = settings_.find(name);
        const auto* before = initial_.find(name);
        changed = !now || !before || json::writeCompact(*now) != json::writeCompact(*before);
    }
    if (!changed) return {};
    json::Value args = json::Value::object();
    args["render"] = projectSettings();
    auto done = dispatcher_.transact("Render settings", kUiOrigin, [&]() -> Result<json::Value> {
        return dispatcher_.execute({"project.set", std::move(args), kUiOrigin});
    });
    if (!done) return std::move(done).error();
    initial_ = settings_;
    initialOwned_ = owned_;
    return {};
}

// ------------------------------------------------------------------ page

void RenderDialog::open() {
    if (page_) return;
    load(json::Value::object());
    presetName_.clear();
    versionChoice_ = "current";
    regions_.reset();
    ask_ = false;
    auto page = buildPage();
    root_.openPopup(std::move(page), {}, ui::Placement::Center, [this] {
        page_ = nullptr;
        body_ = nullptr;
        scroll_ = nullptr;
        status_ = nullptr;
        diagnostic_ = nullptr;
        previewBlock_ = nullptr;
        resultsBlock_ = nullptr;
        resultsRow_ = nullptr;
        queueButton_ = nullptr;
        renderButton_ = nullptr;
        folder_ = nullptr;
        patternField_ = nullptr;
    });
    dirty_ = true;
    lastEditMs_ = nowMs();
    refreshPreview();
}

void RenderDialog::close() {
    if (page_) root_.closePopup();
}

std::unique_ptr<ui::Node> RenderDialog::buildPage() {
    auto page = std::make_unique<ui::Node>("render");
    page_ = page.get();
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.width = ui::SizeRule::fixed(800);
    page->layout.height = ui::SizeRule::fixed(820);
    page->layout.maxHeight = 820;
    page->layout.padding = ui::Insets::all(12);
    page->layout.gap = 6;
    buildHeader(*page);
    scroll_ = &page->emplace<ui::ScrollView>("render.scroll");
    scroll_->layout.height = ui::SizeRule::fraction();
    body_ = &scroll_->content();
    body_->layout.dir = ui::Layout::Dir::Column;
    body_->layout.gap = 4;
    body_->layout.padding = ui::Insets{0, 0, 14, 0};
    buildFooter(*page);
    rebuild();
    return page;
}

void RenderDialog::rebuild() {
    if (!body_) return;
    scrollY_ = scroll_ ? scroll_->scroll() : 0;
    root_.releaseWithin(body_);
    body_->clear();
    patternField_ = nullptr;
    folder_ = nullptr;
    buildSource(*body_);
    buildRange(*body_);
    buildOutput(*body_);
    buildFormat(*body_);
    buildLoudness(*body_);
    buildProcessing(*body_);
    buildMetadata(*body_);
    buildAfter(*body_);
    showFolder();
    refreshQueueButton();
    root_.relayout();
    if (scroll_) scroll_->scrollTo(scrollY_);
    root_.damageAll();
}

void RenderDialog::buildHeader(ui::Node& page) {
    auto& header = row(page, "render.header", 30);
    header.emplace<ui::Label>("render.title", "Render", ui::Font::UiBold, "title");
    auto& gap = header.emplace<ui::Node>("render.header.gap");
    gap.layout.width = ui::SizeRule::fraction();
    caption(header, "render.preset.label", "Preset", 44);
    std::vector<Choice> presets{{"", "—"}};
    if (auto list = dispatcher_.execute({"render.preset.list", json::Value::object(), kUiOrigin}))
        for (const auto& p : (*list)["presets"].asArray()) {
            const std::string name = text(p.find("name"));
            presets.push_back({name, name + (flag(p.find("factory")) ? "" : " (yours)")});
        }
    auto& preset = header.emplace<ui::Dropdown>("render.preset", presets, presetName_);
    preset.layout.width = ui::SizeRule::fixed(250);
    preset.onChange = [this](const std::string& name) {
        if (name.empty()) return;
        if (auto loaded = loadPreset(name); !loaded) setStatus(loaded.error().message);
    };
    header.emplace<ui::Button>("render.preset.save", "Save preset…", [this] { openPresetName(); });
    header.emplace<ui::Button>("render.preset.delete", "Delete preset", [this] {
        if (presetName_.empty()) return;
        if (auto deleted = deletePreset(presetName_); !deleted) setStatus(deleted.error().message);
    });
    header.emplace<ui::Button>("render.close", "Close", [this] { close(); });
}

void RenderDialog::buildSource(ui::Node& body) {
    heading(body, "render.h.source", "Source");
    auto& sources = row(body, "render.sources");
    const Source current = source();
    for (const auto& [s, id, label] :
         {std::tuple{Source::Master, "render.src.master", "Master mix"},
          std::tuple{Source::Stems, "render.src.stems", "Stems"},
          std::tuple{Source::Regions, "render.src.regions", "Regions"}}) {
        auto& t =
            sources.emplace<ui::Toggle>(id, label, "accent", [this, s = s](bool) { setSource(s); });
        t.layout.width = ui::SizeRule::fixed(96);
        t.setOn(current == s);
    }
    auto& gap = sources.emplace<ui::Node>("render.sources.gap");
    gap.layout.width = ui::SizeRule::fixed(20);
    caption(sources, "render.version.label", "Version", 50);
    std::vector<Choice> versions{{"current", "Current"}};
    for (const auto& name : app_.versions().branches())
        if (name != app_.versions().branch())
            versions.push_back({"branch:" + name, "Branch " + name});
    if (versionChoice_.starts_with("version:"))
        versions.push_back({versionChoice_, "Version " + versionChoice_.substr(8)});
    versions.push_back({"earlier", "Earlier version…"});
    auto& version = sources.emplace<ui::Dropdown>("render.version", versions, versionChoice_);
    version.layout.width = ui::SizeRule::fixed(170);
    version.onChange = [this](const std::string& v) {
        if (v == "earlier") {
            chooseVersion();
            return;
        }
        versionChoice_ = v;
        markDirty();
    };
    const model::Session& s = app_.head();
    // Several masters (ADR-0056): which of them the mix renders, a file each.
    if (current == Source::Master && s.project->masters.size() > 1) {
        std::vector<Choice> masters;
        for (model::Id id : s.project->masters) {
            const model::Track* t = s.tracks.find(id);
            masters.push_back({id.str(), t && !t->name.empty() ? t->name : id.str()});
        }
        std::vector<std::string> on;
        if (const auto* list = settings_.find("masters"); list && list->isArray())
            for (const auto& m : list->asArray())
                if (std::any_of(masters.begin(), masters.end(),
                                [&](const Choice& c) { return c.value == m.asString(); }))
                    on.push_back(m.asString());
        if (on.empty()) on.push_back(model::kMaster.str());
        auto& list = body.emplace<ui::Checklist>("render.masters");
        list.setItems(masters, on);
        list.onChange = [this](const std::vector<std::string>& checked) {
            // None checked, or the main master alone: the default.
            if (checked.empty() || checked == std::vector<std::string>{model::kMaster.str()}) {
                set("masters", json::Value(), checked.empty());
                return;
            }
            json::Value ids = json::Value::array();
            for (const auto& id : checked) ids.push_back(id);
            set("masters", std::move(ids));
        };
    }
    if (current == Source::Stems) {
        std::vector<Choice> tracks;
        for (const auto& e : s.tracks)
            if (e.value->parent == model::kMaster)
                tracks.push_back({e.id.str(), e.value->name.empty() ? e.id.str() : e.value->name});
        const json::Value& stems = member("stems");
        std::vector<std::string> on;
        if (const auto* list = stems.find("tracks"); list && list->isArray())
            for (const auto& t : list->asArray()) on.push_back(t.asString());
        else
            for (const auto& t : tracks) on.push_back(t.value);
        auto& list = body.emplace<ui::Checklist>("render.stems.tracks");
        list.setItems(tracks, on);
        list.onChange = [this, count = tracks.size()](const std::vector<std::string>& checked) {
            json::Value st = settings_["stems"];
            if (checked.size() == count)
                st.asObject().erase("tracks");
            else {
                json::Value ids = json::Value::array();
                for (const auto& id : checked) ids.push_back(id);
                st["tracks"] = std::move(ids);
            }
            set("stems", std::move(st));
        };
        auto& opts = row(body, "render.stems.row");
        caption(opts, "render.mode.label", "Mode", 50);
        auto& mode = opts.emplace<ui::Dropdown>(
            "render.mode",
            std::vector<Choice>{{"isolated", "Isolated"}, {"through_master", "Through master"}},
            text(stems.find("mode"), "isolated"));
        mode.quiet = fromDefault("stems");
        mode.onChange = [this](const std::string& v) { setField("render.mode", v); };
        caption(opts, "render.tap.label", "Tap", 30);
        auto& tap = opts.emplace<ui::Dropdown>(
            "render.tap",
            std::vector<Choice>{
                {"post_fader", "Post-fader"}, {"pre_fader", "Pre-fader"}, {"pre_fx", "Pre-FX"}},
            text(stems.find("tap"), "post_fader"));
        tap.quiet = mode.quiet;
        tap.onChange = [this](const std::string& v) { setField("render.tap", v); };
        auto& silent = opts.emplace<ui::Checkbox>("render.skip_silent", "Skip silent stems",
                                                  flag(stems.find("skip_silent")));
        silent.onToggle = [this](bool skip) {
            json::Value st = settings_["stems"];
            st["skip_silent"] = skip;
            set("stems", std::move(st));
        };
    }
    if (current == Source::Regions) {
        std::vector<Choice> regions;
        for (const auto& e : s.regions)
            regions.push_back({e.id.str(), e.value->name.empty() ? e.id.str() : e.value->name});
        auto& list = body.emplace<ui::Checklist>("render.regions.list");
        std::vector<std::string> on;
        for (const auto& r : regions)
            if (!regions_ ||
                std::find(regions_->begin(), regions_->end(), r.value) != regions_->end())
                on.push_back(r.value);
        list.setItems(regions, on);
        list.onChange = [this, count = regions.size()](const std::vector<std::string>& checked) {
            if (checked.size() == count)
                regions_.reset();
            else
                regions_ = checked;
            markDirty();
        };
        auto& opts = row(body, "render.regions.row");
        caption(opts, "render.handles.label", "Handles (s)", 76);
        auto& handles = opts.emplace<ui::EditableDropdown>(
            "render.handles",
            std::vector<Choice>{{"0", "0"}, {"0.5", "0.5"}, {"1", "1"}, {"2", "2"}},
            raw_.count("render.handles") ? raw_["render.handles"]
                                         : compact(num(settings_.find("handles_s"))));
        handles.layout.width = ui::SizeRule::fixed(90);
        handles.onChange = [this](const std::string& t) { setField("render.handles", t); };
        opts.emplace<ui::Button>("render.matrix", "Matrix\u2026", [this] { openMatrix(); });
        std::size_t cells = 0;
        if (const auto* matrix = settings_.find("matrix"); matrix && matrix->isObject())
            for (const auto& row : matrix->asObject()) cells += row.value.size();
        opts.emplace<ui::Label>("render.matrix.note",
                                cells ? std::to_string(cells) +
                                            (cells == 1 ? " track chosen" : " tracks chosen") +
                                            " in the matrix"
                                      : "Matrix: each region renders the master mix",
                                ui::Font::Ui, "caption", "ink-quiet");
    }
}

void RenderDialog::buildRange(ui::Node& body) {
    heading(body, "render.h.range", "Range");
    auto& r = row(body, "render.range.row");
    const std::string bounds = text(settings_.find("bounds"), "project");
    // The time selection is the loop range drawn on the ruler, whether or not
    // looping is on (bounds "loop").
    const auto& loop = app_.loop();
    std::vector<Choice> choices{{"project", "Entire project"},
                                {"loop", "Time selection", loop.end > loop.start},
                                {"custom", "Custom"}};
    auto& range = r.emplace<ui::Dropdown>("render.range", choices, bounds);
    range.layout.width = ui::SizeRule::fixed(150);
    range.quiet = fromDefault("bounds");
    range.onChange = [this](const std::string& v) { set("bounds", v, true); };
    const model::Session& s = app_.head();
    double first = 0, last = model::lastItemBeat(s);
    if (bounds == "loop" && loop.end > loop.start) {
        first = s.timeMap->beatAt(loop.start);
        last = s.timeMap->beatAt(loop.end);
    } else if (bounds == "custom") {
        first = num(settings_.find("start_beat"), 0);
        last = num(settings_.find("end_beat"), last);
    }
    auto position = [&](double beat) {
        const auto bb = s.timeMap->barBeat(beat);
        char buf[48];
        std::snprintf(buf, sizeof buf, "%d.%d", bb.bar, bb.beat);
        return std::string(buf);
    };
    const bool custom = bounds == "custom";
    for (const auto& [id, label, value] :
         {std::tuple{"render.start", "Start", first}, std::tuple{"render.end", "End", last}}) {
        caption(r, std::string(id) + ".label", label, 34);
        auto& field =
            r.emplace<ui::TextField>(id, raw_.count(id) ? raw_[id] : (custom ? compact(value) : ""),
                                     custom ? "beats" : compact(value));
        field.layout.width = ui::SizeRule::fixed(64);
        field.enabled = custom;
        field.onChange = [this, id = std::string(id)](const std::string& t) { setField(id, t); };
        caption(r, std::string(id) + ".bar", position(value), 44);
    }
    char length[64];
    std::snprintf(length, sizeof length, "Length %.2f beats (%.2f s)", std::max(0.0, last - first),
                  std::max(0.0, s.timeMap->secondsAt(last) - s.timeMap->secondsAt(first)));
    r.emplace<ui::Label>("render.length", length, ui::Font::Ui, "caption", "ink-quiet");
    auto& tail = row(body, "render.tail.row");
    const double tailS = num(settings_.find("tail_s"));
    auto& on = tail.emplace<ui::Checkbox>("render.tail.on", "Tail (s)", tailS > 0);
    on.onToggle = [this](bool v) {
        raw_.erase("render.tail");
        set("tail_s", v ? 2.0 : 0.0, true);
    };
    auto& seconds = tail.emplace<ui::EditableDropdown>(
        "render.tail",
        std::vector<Choice>{{"0.5", "0.5"}, {"1", "1"}, {"2", "2"}, {"5", "5"}, {"10", "10"}},
        raw_.count("render.tail") ? raw_["render.tail"] : compact(tailS));
    seconds.layout.width = ui::SizeRule::fixed(90);
    seconds.enabled = tailS > 0 || raw_.count("render.tail");
    seconds.onChange = [this](const std::string& t) { setField("render.tail", t); };
    const json::Value& trim = member("trim");
    auto& cut = tail.emplace<ui::Checkbox>("render.trim.end", "Cut silence at the end",
                                           flag(trim.find("end")));
    cut.onToggle = [this](bool v) {
        json::Value t = settings_["trim"];
        t["end"] = v;
        set("trim", std::move(t));
    };
}

void RenderDialog::buildOutput(ui::Node& body) {
    heading(body, "render.h.output", "Output");
    auto& folderRow = row(body, "render.folder.row");
    caption(folderRow, "render.folder.label", "Folder (all projects)", 130);
    folder_ = &folderRow.emplace<ui::Label>("render.folder", "", ui::Font::Mono, "caption", "ink");
    folder_->layout.width = ui::SizeRule::fraction();
    folderRow.emplace<ui::Button>("render.folder.choose", "Choose folder…", [this] {
        if (auto choose = onChooseFolder) choose();
    });
    folderRow.emplace<ui::Button>("render.folder.open", "Open", [this] {
        if (auto opener = openFolder) opener(app_.rendersDir());
    });
    auto& name = row(body, "render.name.row");
    caption(name, "render.pattern.label", "File name", 130);
    patternField_ = &name.emplace<ui::TextField>("render.pattern", pattern());
    patternField_->layout.width = ui::SizeRule::fraction();
    patternField_->onChange = [this](const std::string& t) { setField("render.pattern", t); };
    patternField_->onSubmit = [this](const std::string&) { refreshPreview(); };
    name.emplace<ui::Button>("render.tokens", "Tokens…", [this] {
        std::vector<ui::MenuItem> items;
        for (const char* token :
             {"{project}", "{version}", "{branch}", "{track}", "{track_number:02}", "{region}",
              "{region_number:02}", "{marker}", "{folder_path}", "{format}", "{sample_rate}",
              "{bit_depth}", "{counter:03}", "{date:YYYY-MM-DD}", "{time:HH-mm-ss}", "/"}) {
            items.push_back({token, "", [this, token = std::string(token)] {
                                 if (!patternField_) return;
                                 const std::string t = patternField_->text();
                                 const std::size_t at = std::min(patternField_->caret(), t.size());
                                 setField("render.pattern", t.substr(0, at) + token + t.substr(at));
                             }});
        }
        if (auto* tokens = root_.find("render.tokens"))
            root_.openPopup(ui::makeMenu("render.tokens.menu", std::move(items),
                                         [this] { root_.closePopup(); }),
                            tokens->bounds());
    });
    auto& exists = row(body, "render.exists.row");
    caption(exists, "render.if_exists.label", "If the file exists", 130);
    auto& ifExists =
        exists.emplace<ui::Dropdown>("render.if_exists",
                                     std::vector<Choice>{{"overwrite", "Overwrite"},
                                                         {"number", "Add a number"},
                                                         {"skip", "Skip"},
                                                         {"ask", "Ask"}},
                                     ask_ ? "ask" : text(settings_.find("if_exists"), "overwrite"));
    ifExists.quiet = fromDefault("if_exists");
    ifExists.onChange = [this](const std::string& v) {
        ask_ = v == "ask";
        if (!ask_) set("if_exists", v);
        markDirty();
    };
}

void RenderDialog::buildFormat(ui::Node& body) {
    heading(body, "render.h.format", "Format");
    auto& outputs = settings_["outputs"].asArray();
    tab_ = std::min(tab_, outputs.size() - 1);
    auto& tabs = row(body, "render.tabs");
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const std::string label =
            (i == 0 ? std::string("Primary") : "Output " + std::to_string(i + 1)) + ": " +
            formatLabel(text(outputs[i].find("format"), "wav"));
        auto& t = tabs.emplace<ui::Toggle>("render.tab." + std::to_string(i), label, "accent",
                                           [this, i](bool) {
                                               tab_ = i;
                                               rebuild();
                                           });
        t.layout.width = ui::SizeRule::fixed(170);
        t.setOn(i == tab_);
        if (i > 0)
            tabs.emplace<ui::Button>("render.tab.remove." + std::to_string(i), "Remove", [this, i] {
                settings_["outputs"].asArray().erase(settings_["outputs"].asArray().begin() +
                                                     static_cast<std::ptrdiff_t>(i));
                tab_ = 0;
                set("outputs", settings_["outputs"], true);
            });
    }
    if (outputs.size() < 4)
        tabs.emplace<ui::Button>("render.tab.add", "+ Output", [this] {
            json::Value o = json::Value::object();
            o["format"] = "mp3";
            settings_["outputs"].push_back(std::move(o));
            tab_ = settings_["outputs"].size() - 1;
            set("outputs", settings_["outputs"], true);
        });
    const json::Value& o = outputs[tab_];
    const std::string format = text(o.find("format"), "wav");
    const bool quiet = fromDefault("outputs");
    auto& line = row(body, "render.format.row");
    caption(line, "render.format.label", "Format", 50);
    auto& f = line.emplace<ui::Dropdown>("render.format", formatChoices(), format);
    f.quiet = quiet;
    f.onChange = [this](const std::string& v) { setField("render.format", v); };
    const auto depths = depthChoices(format);
    const std::int64_t bits = o.find("bit_depth") ? o.find("bit_depth")->asInt() : 0;
    if (!depths.empty()) {
        caption(line, "render.bit_depth.label", "Bit depth", 60);
        auto& d = line.emplace<ui::Dropdown>("render.bit_depth", depths, std::to_string(bits));
        d.quiet = quiet;
        d.onChange = [this](const std::string& v) {
            setField("render.bit_depth", v);
            rebuild();
        };
        if (bits == 16 || bits == 24) {
            caption(line, "render.dither.label", "Dither", 44);
            auto& dither = line.emplace<ui::Dropdown>(
                "render.dither",
                std::vector<Choice>{{"none", "Off"}, {"tpdf", "TPDF"}, {"shaped", "Noise-shaped"}},
                text(o.find("dither"), "tpdf"));
            dither.quiet = quiet;
            dither.onChange = [this](const std::string& v) {
                output(tab_)["dither"] = v;
                set("outputs", settings_["outputs"]);
            };
        }
    }
    auto& rateRow = row(body, "render.rate.row");
    caption(rateRow, "render.sample_rate.label", "Sample rate", 70);
    const std::int64_t projectRate = static_cast<std::int64_t>(app_.head().project->sampleRate);
    std::vector<Choice> rates;
    for (int r : {44100, 48000, 88200, 96000, 176400, 192000})
        rates.push_back({std::to_string(r), std::to_string(r)});
    if (tab_ == 0) {
        const std::int64_t chain =
            settings_.find("sample_rate") ? settings_.find("sample_rate")->asInt() : projectRate;
        const bool own = owned_.count("sample_rate") && chain != projectRate;
        auto& rate = rateRow.emplace<ui::EditableDropdown>(
            "render.sample_rate", rates,
            raw_.count("render.sample_rate") ? raw_["render.sample_rate"]
                                             : (own ? std::to_string(chain) : ""),
            "Project (" + std::to_string(projectRate) + ")");
        rate.layout.width = ui::SizeRule::fixed(170);
        rate.onChange = [this](const std::string& t) { setField("render.sample_rate", t); };
        caption(rateRow, "render.channels.label", "Channels", 56);
        const json::Value* ch = settings_.find("channels");
        const std::string channels = ch && ch->isString()                       ? "track"
                                     : ch && ch->isNumber() && ch->asInt() == 1 ? "1"
                                                                                : "2";
        std::vector<Choice> widths{{"2", "Stereo"}, {"1", "Mono"}};
        if (source() == Source::Stems) widths.push_back({"track", "Mono tracks as mono"});
        auto& c = rateRow.emplace<ui::Dropdown>("render.channels", widths, channels);
        c.quiet = fromDefault("channels");
        c.onChange = [this](const std::string& v) { setField("render.channels", v); };
    } else {
        auto& rate = rateRow.emplace<ui::EditableDropdown>(
            "render.output_rate", rates,
            raw_.count("render.output_rate")
                ? raw_["render.output_rate"]
                : (o.find("sample_rate") ? std::to_string(o.find("sample_rate")->asInt()) : ""),
            "Same as primary");
        rate.layout.width = ui::SizeRule::fixed(140);
        rate.onChange = [this](const std::string& t) { setField("render.output_rate", t); };
        caption(rateRow, "render.suffix.label", "Suffix", 40);
        auto& suffix =
            rateRow.emplace<ui::TextField>("render.suffix", text(o.find("suffix")), "e.g. -16bit");
        suffix.layout.width = ui::SizeRule::fixed(110);
        suffix.onChange = [this](const std::string& t) { setField("render.suffix", t); };
    }
    if (format == "mp3") {
        auto& m = row(body, "render.mp3.row");
        const json::Value* mp3 = o.find("mp3");
        const bool vbr = mp3 && text(mp3->find("mode")) == "vbr";
        caption(m, "render.mp3.mode.label", "Mode", 40);
        auto& mode = m.emplace<ui::Dropdown>(
            "render.mp3.mode", std::vector<Choice>{{"cbr", "Constant"}, {"vbr", "Variable"}},
            vbr ? "vbr" : "cbr");
        mode.onChange = [this](const std::string& v) {
            json::Value opts = json::Value::object();
            opts["mode"] = v;
            if (v == "cbr")
                opts["bitrate_kbps"] = std::int64_t{320};
            else
                opts["vbr_quality"] = std::int64_t{2};
            output(tab_)["mp3"] = std::move(opts);
            set("outputs", settings_["outputs"], true);
        };
        std::vector<Choice> values;
        if (vbr)
            for (int q = 0; q <= 9; ++q)
                values.push_back({std::to_string(q), "V" + std::to_string(q)});
        else
            for (int b : {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320})
                values.push_back({std::to_string(b), std::to_string(b) + " kbps"});
        caption(m, "render.mp3.value.label", vbr ? "Quality" : "Bitrate", 50);
        auto& value = m.emplace<ui::Dropdown>(
            "render.mp3.value", values,
            compact(num(mp3 ? mp3->find(vbr ? "vbr_quality" : "bitrate_kbps") : nullptr,
                        vbr ? 2 : 320)));
        value.onChange = [this, vbr](const std::string& v) {
            json::Value opts = output(tab_)["mp3"];
            opts[vbr ? "vbr_quality" : "bitrate_kbps"] = integer(v).value_or(vbr ? 2 : 320);
            output(tab_)["mp3"] = std::move(opts);
            set("outputs", settings_["outputs"]);
        };
    }
    if (format == "vorbis") {
        auto& v = row(body, "render.vorbis.row");
        caption(v, "render.vorbis.label", "Quality (−1 to 10)", 110);
        std::vector<Choice> qualities;
        for (int q = -1; q <= 10; ++q) qualities.push_back({std::to_string(q), std::to_string(q)});
        auto& q = v.emplace<ui::EditableDropdown>("render.vorbis_quality", qualities,
                                                  raw_.count("render.vorbis_quality")
                                                      ? raw_["render.vorbis_quality"]
                                                      : compact(num(o.find("vorbis_quality"), 6)));
        q.layout.width = ui::SizeRule::fixed(90);
        q.onChange = [this](const std::string& t) { setField("render.vorbis_quality", t); };
    }
}

void RenderDialog::buildLoudness(ui::Node& body) {
    const json::Value& n = member("normalize");
    const std::string mode = text(n.find("mode"), "none");
    const double target = num(n.find("target"), -14);
    const json::Value* limit = settings_.find("limit_true_peak_db");
    const bool set_ =
        mode != "none" || (limit && limit->isNumber()) || flag(settings_.find("report"));
    auto& fold = body.emplace<ui::FoldSection>("render.loudness", "Loudness", set_);
    std::string summary;
    if (mode != "none") summary = signedText(target) + (mode.starts_with("lufs") ? " LUFS" : " dB");
    if (limit && limit->isNumber())
        summary += (summary.empty() ? "" : ", ") + signedText(limit->asDouble()) + " dBTP";
    fold.setSummary(summary.empty() ? "off" : summary);
    fold.onToggle = [this](bool) { root_.relayout(); };
    ui::Node& b = fold.body();
    auto& r1 = row(b, "render.normalize.row");
    caption(r1, "render.normalize.label", "Normalize", 70);
    auto& m =
        r1.emplace<ui::Dropdown>("render.normalize",
                                 std::vector<Choice>{{"none", "Off"},
                                                     {"lufs_i", "Integrated loudness (LUFS-I)"},
                                                     {"lufs_s", "Loudest 3 s (LUFS-S)"},
                                                     {"lufs_m", "Loudest 400 ms (LUFS-M)"},
                                                     {"peak", "Sample peak"},
                                                     {"true_peak", "True peak"},
                                                     {"rms", "RMS"}},
                                 mode);
    m.quiet = fromDefault("normalize");
    m.onChange = [this](const std::string& v) {
        json::Value next = settings_["normalize"];
        next["mode"] = v;
        const double t = num(next.find("target"), -14);
        if (v.starts_with("lufs") && t > -5) next["target"] = -14.0;
        if ((v == "peak" || v == "true_peak") && t < -12) next["target"] = -1.0;
        if (v == "rms" && (t < -40 || t > -6)) next["target"] = -20.0;
        raw_.erase("render.target");
        set("normalize", std::move(next), true);
    };
    if (mode != "none") {
        caption(r1, "render.target.label", "Target", 44);
        std::vector<Choice> targets;
        if (mode.starts_with("lufs"))
            targets = {{"-14", "−14 (Spotify, YouTube)"},
                       {"-16", "−16 (Apple Music, podcasts)"},
                       {"-23", "−23 (EBU R128)"},
                       {"-24", "−24 (ATSC A/85)"}};
        else if (mode == "rms")
            targets = {{"-20", "−20"}, {"-16", "−16"}};
        else
            targets = {{"-1", "−1"}, {"-0.1", "−0.1"}};
        auto& t = r1.emplace<ui::EditableDropdown>(
            "render.target", targets,
            raw_.count("render.target") ? raw_["render.target"] : compact(target));
        t.layout.width = ui::SizeRule::fixed(90);
        t.onChange = [this](const std::string& v) { setField("render.target", v); };
        r1.emplace<ui::Label>("render.target.unit", mode.starts_with("lufs") ? "LUFS" : "dB",
                              ui::Font::Ui, "caption", "ink-quiet");
    }
    auto& r2 = row(b, "render.normalize.opts");
    auto& reduce = r2.emplace<ui::Checkbox>("render.only_reduce", "Only turn down",
                                            flag(n.find("only_reduce")));
    reduce.enabled = mode != "none";
    reduce.onToggle = [this](bool v) {
        json::Value next = settings_["normalize"];
        next["only_reduce"] = v;
        set("normalize", std::move(next));
    };
    if (source() == Source::Stems) {
        auto& shared = r2.emplace<ui::Checkbox>("render.scope",
                                                "Same gain for every stem (from the master mix)",
                                                text(n.find("scope")) == "master");
        shared.enabled = mode != "none";
        shared.onToggle = [this](bool v) {
            json::Value next = settings_["normalize"];
            next["scope"] = v ? "master" : "file";
            set("normalize", std::move(next));
        };
    }
    auto& r3 = row(b, "render.limit.row");
    const bool limited = limit && limit->isNumber();
    auto& lim = r3.emplace<ui::Checkbox>("render.limit.on", "Limit true peak to", limited);
    lim.onToggle = [this](bool v) {
        raw_.erase("render.limit");
        set("limit_true_peak_db", v ? json::Value(-1.0) : json::Value(), true);
    };
    auto& ceiling = r3.emplace<ui::EditableDropdown>(
        "render.limit",
        std::vector<Choice>{{"-1", "−1"}, {"-0.5", "−0.5"}, {"-0.1", "−0.1"}, {"-2", "−2"}},
        raw_.count("render.limit") ? raw_["render.limit"]
                                   : (limited ? compact(limit->asDouble()) : ""));
    ceiling.layout.width = ui::SizeRule::fixed(90);
    ceiling.onChange = [this](const std::string& v) { setField("render.limit", v); };
    r3.emplace<ui::Label>("render.limit.unit", "dBTP", ui::Font::Ui, "caption", "ink-quiet");
    auto& report = r3.emplace<ui::Checkbox>("render.report", "Write a loudness report",
                                            flag(settings_.find("report")));
    report.onToggle = [this](bool v) { set("report", v); };
}

void RenderDialog::buildProcessing(ui::Node& body) {
    const json::Value& trim = member("trim");
    const bool seamless = flag(settings_.find("seamless_loop"));
    const bool set_ = flag(trim.find("start")) || flag(trim.find("end")) || seamless;
    auto& fold = body.emplace<ui::FoldSection>("render.processing", "Processing", set_);
    std::string summary;
    if (flag(trim.find("start")) || flag(trim.find("end"))) summary = "trim";
    if (seamless) summary += (summary.empty() ? "" : ", ") + std::string("seamless loop");
    fold.setSummary(summary.empty() ? "off" : summary);
    fold.onToggle = [this](bool) { root_.relayout(); };
    auto& r = row(fold.body(), "render.trim.row");
    for (const auto& [id, key, label] : {std::tuple{"render.trim.start", "start", "Trim start"},
                                         std::tuple{"render.trim.end2", "end", "Trim end"}}) {
        auto& box = r.emplace<ui::Checkbox>(id, label, flag(trim.find(key)));
        box.onToggle = [this, key = std::string(key)](bool v) {
            json::Value t = settings_["trim"];
            t[key] = v;
            set("trim", std::move(t), true);
        };
    }
    caption(r, "render.threshold.label", "below (dBFS)", 70);
    auto& threshold = r.emplace<ui::EditableDropdown>(
        "render.threshold", std::vector<Choice>{{"-60", "−60"}, {"-70", "−70"}, {"-50", "−50"}},
        raw_.count("render.threshold") ? raw_["render.threshold"]
                                       : compact(num(trim.find("threshold_db"), -60)));
    threshold.layout.width = ui::SizeRule::fixed(90);
    threshold.onChange = [this](const std::string& v) { setField("render.threshold", v); };
    auto& loop = fold.body().emplace<ui::Checkbox>(
        "render.seamless", "Seamless loop (fold the tail into the start)", seamless);
    loop.onToggle = [this](bool v) { set("seamless_loop", v, true); };
}

void RenderDialog::buildMetadata(ui::Node& body) {
    const json::Value* m = settings_.find("metadata");
    const bool set_ = m && m->isObject() && m->size() > 0;
    auto& fold = body.emplace<ui::FoldSection>("render.metadata", "Metadata", set_);
    fold.setSummary(set_ ? text(m->find("title"), "set") : "none");
    fold.onToggle = [this](bool) { root_.relayout(); };
    std::set<std::string> formats;
    for (const auto& o : settings_["outputs"].asArray()) formats.insert(text(o.find("format")));
    // Which of the chosen formats cannot hold a member (engine.md §3).
    auto missing = [&](const std::string& key) {
        std::string out;
        auto lacks = [&](const std::string& format) {
            if (format == "aiff") return key != "title" && key != "artist" && key != "comment";
            if (format == "wav") return key == "isrc" || key == "cover";
            return false;
        };
        for (const auto& f : formats)
            if (lacks(f)) out += (out.empty() ? "not in " : ", ") + formatLabel(f);
        return out;
    };
    for (const auto& [key, label] : {std::pair{"title", "Title"},
                                     {"artist", "Artist"},
                                     {"album", "Album"},
                                     {"track_number", "Track number"},
                                     {"date", "Date"},
                                     {"genre", "Genre"},
                                     {"isrc", "ISRC"},
                                     {"comment", "Comment"}}) {
        const std::string id = "render.meta." + std::string(key);
        auto& r = row(fold.body(), id + ".row");
        caption(r, id + ".label", label, 90);
        std::string value = raw_.count(id) ? raw_[id] : "";
        if (!raw_.count(id) && m)
            if (const auto* v = m->find(key))
                value = v->isString() ? v->asString() : compact(v->asDouble());
        auto& field = r.emplace<ui::TextField>(id, value,
                                               std::string(key) == "date" ? "YYYY or YYYY-MM-DD"
                                               : std::string(key) == "isrc"
                                                   ? "e.g. USRC17607839"
                                                   : "tokens allowed, e.g. {project}");
        field.layout.width = ui::SizeRule::fixed(320);
        field.onChange = [this, id](const std::string& t) { setField(id, t); };
        r.emplace<ui::Label>(id + ".note", missing(key), ui::Font::Ui, "caption", "ink-faint");
    }
    auto& cover = row(fold.body(), "render.cover.row");
    caption(cover, "render.cover.label", "Cover", 90);
    cover.emplace<ui::Label>("render.cover", m ? text(m->find("cover"), "none") : "none",
                             ui::Font::Mono, "caption", "ink");
    cover.emplace<ui::Button>("render.cover.choose", "Choose…", [this] {
        if (auto choose = onChooseCover) choose();
    });
    cover.emplace<ui::Button>("render.cover.clear", "Clear", [this] { setCover(""); });
    cover.emplace<ui::Label>("render.cover.note", missing("cover"), ui::Font::Ui, "caption",
                             "ink-faint");
    auto& flags = row(fold.body(), "render.meta.flags");
    for (const auto& [key, label] :
         {std::pair{"bwf", "Write BWF timecode and loudness (WAV)"},
          std::pair{"markers", "Embed markers and regions (WAV, AIFF)"}}) {
        auto& box = flags.emplace<ui::Checkbox>("render.meta." + std::string(key), label,
                                                m && flag(m->find(key)));
        box.onToggle = [this, key = std::string(key)](bool v) {
            json::Value next =
                settings_.find("metadata") ? settings_["metadata"] : json::Value::object();
            if (v)
                next[key] = true;
            else
                next.asObject().erase(key);
            set("metadata", next.size() ? std::move(next) : json::Value());
        };
    }
}

void RenderDialog::buildAfter(ui::Node& body) {
    heading(body, "render.h.after", "After render");
    auto& r = row(body, "render.after.row");
    auto& add = r.emplace<ui::Checkbox>("render.add", "Add to project", addToProject());
    add.onToggle = [this](bool v) { set("add_to_project", v); };
    auto& open = r.emplace<ui::Checkbox>("render.open_folder", "Open folder when done",
                                         openFolderWhenDone && openFolderWhenDone());
    open.onToggle = [this](bool v) {
        if (auto setPreference = setOpenFolderWhenDone) setPreference(v);
    };
}

void RenderDialog::buildFooter(ui::Node& page) {
    diagnostic_ =
        &page.emplace<ui::Label>("render.diagnostic", "", ui::Font::Ui, "caption", "clip");
    // The file lists scroll inside fixed heights: stems and regions can list
    // many files, which must not spill over the buttons below.
    auto& previewScroll = page.emplace<ui::ScrollView>("render.preview.scroll");
    previewScroll.layout.height = ui::SizeRule::fixed(72);
    previewBlock_ = &previewScroll.content().emplace<ui::TextBlock>(
        "render.preview", "", ui::Font::Mono, "caption", "ink");
    auto& actions = row(page, "render.actions", 28);
    queueButton_ = &actions.emplace<ui::Button>("render.queue", "Queue", [this] { openQueue(); });
    actions.emplace<ui::Button>("render.queue.add", "Add to queue", [this] { addToQueue(); });
    actions.emplace<ui::Button>("render.default", "Save as default", [this] {
        if (auto saved = saveAsDefault(); !saved)
            setStatus(saved.error().message);
        else
            setStatus("saved as your default");
    });
    status_ = &actions.emplace<ui::Label>("render.status", statusText_, ui::Font::Ui, "caption",
                                          "ink-quiet");
    status_->layout.width = ui::SizeRule::fraction();
    renderButton_ = &actions.emplace<ui::Button>("render.run", "Render", [this] { run(); });
    auto& resultsScroll = page.emplace<ui::ScrollView>("render.results.scroll");
    resultsScroll.layout.height = ui::SizeRule::fixed(72);
    resultsBlock_ = &resultsScroll.content().emplace<ui::TextBlock>(
        "render.results", "", ui::Font::Mono, "caption", "ink-quiet");
    resultsRow_ = &row(page, "render.results.files", 24);
}

void RenderDialog::showFolder() {
    std::string path = app_.rendersDir().generic_string();
    if (const auto home = paths::home(); !home.empty()) {
        const std::string prefix = home.generic_string() + "/";
        if (path.starts_with(prefix)) path = "~/" + path.substr(prefix.size());
    }
    folderText_ = path;
    if (folder_) folder_->setText(path);
    root_.damageAll();
    markDirty();  // the preview's paths change with it
}

// ------------------------------------------------------------------ popups

void RenderDialog::openPresetName() {
    auto panel = std::make_unique<ui::Node>("render.preset.panel");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fixed(360);
    panel->layout.padding = ui::Insets::all(12);
    panel->layout.gap = 8;
    panel->emplace<ui::Label>("render.preset.title", "Save preset", ui::Font::UiBold, "title");
    auto& name = panel->emplace<ui::TextField>("render.preset.name", presetName_, "Preset name");
    auto& buttons = row(*panel, "render.preset.buttons");
    auto save = [this, &name] {
        const std::string chosen = name.text();
        root_.closePopup();
        if (auto saved = savePreset(chosen); !saved)
            setStatus(saved.error().message);
        else
            setStatus("saved preset " + chosen);
    };
    name.onSubmit = [save](const std::string&) { save(); };
    buttons.emplace<ui::Button>("render.preset.ok", "Save", save);
    buttons.emplace<ui::Button>("render.preset.cancel", "Cancel", [this] { root_.closePopup(); });
    root_.openPopup(std::move(panel), {}, ui::Placement::Center);
    root_.focus(root_.find("render.preset.name"));
}

void RenderDialog::chooseVersion() {
    auto history = dispatcher_.execute({"history.list", json::Value::object(), kUiOrigin});
    std::vector<ui::MenuItem> items;
    if (history)
        if (const auto* versions = history->find("versions"); versions && versions->isArray())
            for (const auto& v : versions->asArray()) {
                const auto n = v.find("version") ? v.find("version")->asInt() : 0;
                items.push_back(
                    {"v" + std::to_string(n) + "  " + text(v.find("label")), "", [this, n] {
                         versionChoice_ = "version:" + std::to_string(n);
                         markDirty();
                         rebuild();
                     }});
            }
    if (items.empty()) items.push_back({"No earlier versions", "", {}, false});
    if (auto* anchor = root_.find("render.version"))
        root_.openPopup(
            ui::makeMenu("render.version.menu", std::move(items), [this] { root_.closePopup(); }),
            anchor->bounds());
    rebuild();  // the dropdown shows the choice again until one is made
}

void RenderDialog::openExisting(const json::Value& existing) {
    auto panel = std::make_unique<ui::Node>("render.exists.panel");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fixed(460);
    panel->layout.padding = ui::Insets::all(12);
    panel->layout.gap = 8;
    panel->emplace<ui::Label>("render.exists.title", "These files exist", ui::Font::UiBold,
                              "title");
    std::string names;
    for (const auto& f : existing.asArray())
        names += (names.empty() ? "" : "\n") + baseName(text(f.find("path")));
    auto& list = panel->emplace<ui::TextBlock>("render.exists.list", names, ui::Font::Mono,
                                               "caption", "ink");
    list.layout.height = ui::SizeRule::fixed(120);
    auto& buttons = row(*panel, "render.exists.buttons");
    for (const auto& [id, label, mode] :
         {std::tuple{"render.exists.overwrite", "Overwrite", "overwrite"},
          std::tuple{"render.exists.skip", "Skip them", "skip"}})
        buttons.emplace<ui::Button>(id, label, [this, mode = std::string(mode)] {
            root_.closePopup();
            if (auto written = writeSettings(); !written) {
                setStatus("render settings: " + written.error().message);
                return;
            }
            json::Value a = renderArgs(true);
            a["if_exists"] = mode;
            startJob(commandName(), std::move(a));
        });
    buttons.emplace<ui::Button>("render.exists.cancel", "Cancel", [this] { root_.closePopup(); });
    root_.openPopup(std::move(panel), {}, ui::Placement::Center);
}

void RenderDialog::openMatrix() {
    const model::Session& s = app_.head();
    std::vector<std::pair<std::string, std::string>> rows{{"project", "Project"}};
    for (const auto& e : s.regions)
        rows.emplace_back(e.id.str(), e.value->name.empty() ? e.id.str() : e.value->name);
    std::vector<std::pair<std::string, std::string>> columns;
    for (const auto& e : s.tracks)
        if (e.value->type == model::StripType::Master || e.value->parent == model::kMaster)
            columns.emplace_back(e.id.str(), e.value->name.empty() ? e.id.str() : e.value->name);
    auto panel = std::make_unique<ui::Node>("render.matrix.panel");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fixed(
        std::min(1100.0f, 140.0f + 90.0f * static_cast<float>(columns.size()) + 24));
    panel->layout.padding = ui::Insets::all(12);
    panel->layout.gap = 4;
    panel->emplace<ui::Label>("render.matrix.title", "Render matrix: regions by tracks",
                              ui::Font::UiBold, "title");
    auto& head = row(*panel, "render.matrix.head");
    caption(head, "render.matrix.corner", "", 140);
    for (const auto& [id, name] : columns) caption(head, "render.matrix.col." + id, name, 84);
    const json::Value* matrix = settings_.find("matrix");
    for (const auto& [rowId, rowName] : rows) {
        auto& line = row(*panel, "render.matrix.row." + rowId);
        caption(line, "render.matrix.name." + rowId, rowName, 140);
        const json::Value* cells = matrix && matrix->isObject() ? matrix->find(rowId) : nullptr;
        for (const auto& [trackId, trackName] : columns) {
            bool on = false;
            if (cells && cells->isArray())
                for (const auto& c : cells->asArray())
                    on |= c.isString() && c.asString() == trackId;
            auto& box =
                line.emplace<ui::Checkbox>("render.matrix." + rowId + "." + trackId, "", on);
            box.layout.width = ui::SizeRule::fixed(84);
            box.onToggle = [this, rowId = rowId, trackId = trackId](bool checked) {
                json::Value next = settings_.find("matrix") && settings_["matrix"].isObject()
                                       ? settings_["matrix"]
                                       : json::Value::object();
                json::Value cellsNow = next.find(rowId) ? next[rowId] : json::Value::array();
                json::Value kept = json::Value::array();
                for (const auto& c : cellsNow.asArray())
                    if (c.asString() != trackId) kept.push_back(c);
                if (checked) kept.push_back(trackId);
                if (kept.size())
                    next[rowId] = std::move(kept);
                else
                    next.asObject().erase(rowId);
                set("matrix", next.size() ? std::move(next) : json::Value());
            };
        }
    }
    auto& buttons = row(*panel, "render.matrix.buttons");
    buttons.emplace<ui::Button>("render.matrix.close", "Done", [this] {
        root_.closePopup();
        rebuild();
    });
    root_.openPopup(std::move(panel), {}, ui::Placement::Center);
}

// ------------------------------------------------------------------ presets

Status RenderDialog::loadPreset(const std::string& name) {
    auto list = dispatcher_.execute({"render.preset.list", json::Value::object(), kUiOrigin});
    if (!list) return std::move(list).error();
    for (const auto& p : (*list)["presets"].asArray())
        if (text(p.find("name")) == name) {
            json::Value args = *p.find("settings");
            if (!args.size()) args["source"] = text(settings_.find("source"), "mix");
            load(args);
            presetName_ = name;
            markDirty();
            rebuild();
            setStatus("loaded preset " + name + "; Render writes it to the project");
            return {};
        }
    return Error("render.preset_unknown", "no preset " + name, "/name",
                 "choose one from the Preset list");
}

Status RenderDialog::savePreset(const std::string& name) {
    json::Value a = json::Value::object();
    a["name"] = name;
    a["settings"] = settings_;
    auto saved = dispatcher_.execute({"render.preset.save", std::move(a), kUiOrigin});
    if (!saved) return std::move(saved).error();
    presetName_ = name;
    if (page_) rebuild();
    return {};
}

Status RenderDialog::saveAsDefault() {
    json::Value a = json::Value::object();
    a["default"] = true;
    a["settings"] = settings_;
    auto saved = dispatcher_.execute({"render.preset.save", std::move(a), kUiOrigin});
    if (!saved) return std::move(saved).error();
    return {};
}

Status RenderDialog::deletePreset(const std::string& name) {
    json::Value a = json::Value::object();
    a["name"] = name;
    auto deleted = dispatcher_.execute({"render.preset.delete", std::move(a), kUiOrigin});
    if (!deleted) return std::move(deleted).error();
    presetName_.clear();
    if (page_) rebuild();
    setStatus("deleted preset " + name);
    return {};
}

// ------------------------------------------------------------------ preview and jobs

void RenderDialog::refreshPreview() {
    dirty_ = false;
    if (auto valid = validate(); !valid) {
        previewText_ = valid.error().message;
        setStatus(previewText_);
        if (diagnostic_) diagnostic_->setText(previewText_);
        if (previewBlock_) previewBlock_->setText(previewText_);
        return;
    }
    if (diagnostic_) diagnostic_->setText("");
    json::Value args = renderArgs(false);
    args.asObject().erase("analyze");
    args["operation"] = sourceKey(source());
    auto result = dispatcher_.execute({"render.dry-run", std::move(args), kUiOrigin});
    if (!result) {
        std::string where = result.error().path;
        if (where.starts_with("/args")) where = where.substr(5);
        previewText_ =
            (where.empty() ? std::string() : where.substr(1) + ": ") + result.error().message;
        setStatus(previewText_);
        if (diagnostic_) diagnostic_->setText(previewText_);
        if (previewBlock_) previewBlock_->setText(previewText_);
        if (renderButton_) renderButton_->setText("Render");
        return;
    }
    const json::Value* files = result->find("files");
    std::string lines;
    std::size_t count = 0;
    if (files && files->isArray())
        for (const json::Value& f : files->asArray()) {
            std::string kinds;
            if (const auto* outputs = f.find("outputs"); outputs && outputs->isArray())
                for (const auto& o : outputs->asArray())
                    kinds += (kinds.empty() ? "" : " + ") + formatLabel(text(o.find("format")));
            char line[512];
            std::snprintf(line, sizeof line, "%s  (%.2f s)  %s%s",
                          baseName(text(f.find("path"))).c_str(), num(f.find("seconds")),
                          kinds.c_str(),
                          flag(f.find("skip"))     ? "  exists: skipped"
                          : flag(f.find("exists")) ? "  exists"
                                                   : "");
            lines += (lines.empty() ? "" : "\n") + std::string(line);
            if (!flag(f.find("skip"))) ++count;
        }
    if (lines.empty()) lines = "no files";
    previewText_ = lines;
    setStatus(std::to_string(count) + (count == 1 ? " file" : " files"));
    if (previewBlock_) previewBlock_->setText(previewText_);
    if (renderButton_)
        renderButton_->setText("Render " + std::to_string(count) +
                               (count == 1 ? " file" : " files"));
    root_.damageAll();
}

void RenderDialog::startJob(std::string command, json::Value args) {
    auto result = dispatcher_.execute({command, std::move(args), kUiOrigin});
    if (!result) {
        setStatus(command + ": " + result.error().message);
        return;
    }
    if (const json::Value* id = result->find("job"); id && id->isString()) {
        job_ = id->asString();
        resultFiles_.clear();
        setStatus("rendering…");
        if (resultsBlock_) resultsBlock_->setText("");
    } else {
        job_.clear();
        showResults(*result, false);
    }
}

void RenderDialog::run() {
    if (!job_.empty()) {
        setStatus("a render job is already active; wait for it to finish");
        return;
    }
    if (auto valid = validate(); !valid) {
        setStatus(valid.error().message);
        return;
    }
    if (ask_) {
        json::Value args = renderArgs(false);
        args.asObject().erase("analyze");
        args["operation"] = sourceKey(source());
        args["if_exists"] = "overwrite";
        auto dry = dispatcher_.execute({"render.dry-run", std::move(args), kUiOrigin});
        if (!dry) {
            setStatus(dry.error().message);
            return;
        }
        json::Value existing = json::Value::array();
        for (const auto& f : (*dry)["files"].asArray())
            if (flag(f.find("exists"))) existing.push_back(f);
        if (existing.size()) {
            openExisting(existing);
            return;
        }
    }
    if (auto written = writeSettings(); !written) {
        setStatus("render settings: " + written.error().message);
        return;
    }
    mixJob_ = false;
    startJob(commandName(), renderArgs(true));
}

void RenderDialog::addToQueue() {
    if (auto valid = validate(); !valid) {
        setStatus(valid.error().message);
        return;
    }
    if (auto written = writeSettings(); !written) {
        setStatus("render settings: " + written.error().message);
        return;
    }
    json::Value a = renderArgs(false);
    a.asObject().erase("analyze");
    json::Value q = json::Value::object();
    q["command"] = commandName();
    q["args"] = std::move(a);
    auto result = dispatcher_.execute({"render.queue.add", std::move(q), kUiOrigin});
    if (!result) {
        setStatus("queue: " + result.error().message);
        return;
    }
    setStatus("queued " + text(result->find("label")));
    refreshQueueButton();
}

void RenderDialog::refreshQueueButton() {
    if (!queueButton_) return;
    const std::size_t n = app_.renderQueue().size();
    queueButton_->setText(n ? "Queue (" + std::to_string(n) + ")" : "Queue");
}

void RenderDialog::openQueue() {
    auto panel = std::make_unique<ui::Node>("render.queue.panel");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fixed(520);
    panel->layout.padding = ui::Insets::all(12);
    panel->layout.gap = 8;
    panel->emplace<ui::Label>("render.queue.title", "Render queue", ui::Font::UiBold, "title");
    std::string lines;
    for (const auto& q : app_.renderQueue()) {
        lines += (lines.empty() ? "" : "\n") + q.id + "  " + q.label;
        if (q.files.isArray())
            for (const json::Value& f : q.files.asArray())
                if (f.isString()) lines += "\n    " + baseName(f.asString());
    }
    auto& list = panel->emplace<ui::TextBlock>(
        "render.queue.list", lines.empty() ? "Nothing queued: Add to queue sets one up." : lines,
        ui::Font::Mono, "caption", "ink");
    list.layout.height = ui::SizeRule::fixed(180);
    auto& buttons = row(*panel, "render.queue.buttons");
    auto& go = buttons.emplace<ui::Button>("render.queue.run", "Render all", [this] {
        root_.closePopup();
        runQueue();
    });
    go.enabled = !app_.renderQueue().empty();
    buttons.emplace<ui::Button>("render.queue.clear", "Clear", [this] {
        json::Value a = json::Value::object();
        a["all"] = true;
        (void)dispatcher_.execute({"render.queue.remove", std::move(a), kUiOrigin});
        root_.closePopup();
        refreshQueueButton();
        setStatus("queue cleared");
    });
    buttons.emplace<ui::Button>("render.queue.close", "Close", [this] { root_.closePopup(); });
    root_.openPopup(std::move(panel), {}, ui::Placement::Center);
}

void RenderDialog::runQueue() {
    if (!job_.empty()) {
        setStatus("a render job is already active; wait for it to finish");
        return;
    }
    json::Value a = json::Value::object();
    a["job"] = true;
    mixJob_ = false;
    startJob("render.queue.run", std::move(a));
    if (!job_.empty()) setStatus("rendering the queue…");
}

void RenderDialog::mixJob() {
    if (!job_.empty()) {
        if (auto status = onStatus) status("a render is already running");
        return;
    }
    json::Value a = json::Value::object();
    a["job"] = true;
    auto result = dispatcher_.execute({"render.mix", std::move(a), kUiOrigin});
    if (!result) {
        if (auto status = onStatus) status("Mix: " + result.error().message);
        return;
    }
    if (const json::Value* id = result->find("job"); id && id->isString()) {
        job_ = id->asString();
        mixJob_ = true;
        if (auto status = onStatus) status("Mixing…");
    }
}

void RenderDialog::showResults(const json::Value& result, bool cancelled) {
    resultFiles_.clear();
    results_ = json::Value::array();
    std::string lines;
    json::Value skipped = json::Value::array();
    auto addFile = [&](const json::Value& entry) {
        const std::string path = text(entry.find("path"));
        if (path.empty()) return;
        resultFiles_.push_back(path);
        results_.push_back(entry);
        char head[320];
        std::snprintf(head, sizeof head, "%s  (%.2f s)", baseName(path).c_str(),
                      num(entry.find("seconds")));
        std::string line = head;
        if (const json::Value* analysis = entry.find("analysis"); analysis && analysis->isObject())
            if (const json::Value* loudness = analysis->find("loudness");
                loudness && loudness->isObject()) {
                for (const auto& [key, unit] :
                     {std::pair{"integrated_lufs", "LUFS"}, std::pair{"true_peak_dbtp", "dBTP"}}) {
                    const json::Value* v = loudness->find(key);
                    if (!v || !v->isNumber()) continue;
                    char part[48];
                    std::snprintf(part, sizeof part, "  %.1f %s", v->asDouble(), unit);
                    line += part;
                }
            }
        if (const auto* outputs = entry.find("outputs"); outputs && outputs->isArray())
            for (const auto& o : outputs->asArray()) {
                line += "\n    " + baseName(text(o.find("path")));
                if (const auto* checks = o.find("checks"); checks && checks->isObject())
                    for (const auto& [key, unit] :
                         {std::pair{"loudness", "LUFS"}, std::pair{"true_peak", "dBTP"}})
                        if (const auto* c = checks->find(key)) {
                            char part[64];
                            std::snprintf(part, sizeof part, "  %s %.1f %s",
                                          flag(c->find("ok")) ? "ok" : "FAIL",
                                          num(c->find("measured"), -1000), unit);
                            line += part;
                        }
            }
        lines += (lines.empty() ? "" : "\n") + line;
    };
    auto addSkipped = [&](const json::Value* list) {
        if (list && list->isArray())
            for (const auto& s : list->asArray()) skipped.push_back(s);
    };
    if (const json::Value* renders = result.find("renders"); renders && renders->isArray()) {
        for (const json::Value& r : renders->asArray()) {
            if (const json::Value* files = r.find("files"); files && files->isArray())
                for (const json::Value& f : files->asArray()) addFile(f);
            addSkipped(r.find("skipped"));
        }
    } else if (const json::Value* files = result.find("files"); files && files->isArray())
        for (const json::Value& f : files->asArray()) addFile(f);
    else if (result.find("path"))
        addFile(result);
    addSkipped(result.find("skipped"));
    for (const auto& s : skipped.asArray())
        lines += (lines.empty() ? "" : "\n") + baseName(text(s.find("path"))) + "  skipped (" +
                 text(s.find("reason")) + ")";
    setStatus((cancelled ? "cancelled; " : "") + std::to_string(resultFiles_.size()) +
              (resultFiles_.size() == 1 ? " file rendered" : " files rendered"));
    refreshQueueButton();
    if (resultsBlock_) resultsBlock_->setText(lines.empty() ? "no files" : lines);
    if (resultsRow_) {
        root_.releaseWithin(resultsRow_);
        resultsRow_->clear();
        // One file picker and its actions, however many files there are.
        if (!resultFiles_.empty()) {
            std::vector<ui::Choice> files;
            for (std::size_t i = 0; i < resultFiles_.size(); ++i)
                files.push_back({std::to_string(i), baseName(resultFiles_[i])});
            auto& pick =
                resultsRow_->emplace<ui::Dropdown>("render.result.file", std::move(files), "0");
            pick.layout.width = ui::SizeRule::fixed(360);
            resultsRow_->emplace<ui::Button>("render.result.actions", "File actions…", [this] {
                const auto* picked = dynamic_cast<ui::Dropdown*>(root_.find("render.result.file"));
                const auto* button = root_.find("render.result.actions");
                if (!picked || !button) return;
                std::size_t index = 0;
                const std::string& v = picked->value();
                std::from_chars(v.data(), v.data() + v.size(), index);
                openResultMenu(index, button->bounds());
            });
        }
        root_.relayout();
    }
    root_.damageAll();
}

void RenderDialog::openResultMenu(std::size_t index, const ui::Rect& anchor) {
    if (index >= results_.size()) return;
    const json::Value entry = results_.at(index);
    const std::filesystem::path path = text(entry.find("path"));
    std::vector<ui::MenuItem> items;
    items.push_back({"Open folder", "", [this, path] {
                         if (auto opener = openFolder) opener(path.parent_path());
                     }});
    items.push_back(
        {"Add to project", "", [this, entry] {
             auto done = dispatcher_.transact(
                 "Add render to project", kUiOrigin, [&]() -> Result<json::Value> {
                     json::Value track = json::Value::object();
                     track["name"] =
                         std::filesystem::path(text(entry.find("path"))).stem().string();
                     auto t = dispatcher_.execute({"track.add", track, kUiOrigin});
                     if (!t) return t;
                     json::Value item = json::Value::object();
                     item["track"] = (*t)["track"];
                     item["position"] = num(entry.find("start_beat"));
                     item["audio_file"] = text(entry.find("path"));
                     return dispatcher_.execute({"item.add", item, kUiOrigin});
                 });
             setStatus(done ? "added to the project" : done.error().message);
         }});
    const auto version = entry.find("version") ? entry.find("version")->asInt() : 0;
    items.push_back({"Branch from this version", "",
                     [this, version] {
                         for (int n = 1; n < 100; ++n) {
                             json::Value a = json::Value::object();
                             a["name"] = "from-v" + std::to_string(version) +
                                         (n > 1 ? "-" + std::to_string(n) : "");
                             a["version"] = version;
                             auto made = dispatcher_.execute({"branch.create", a, kUiOrigin});
                             if (made) {
                                 setStatus("switched to branch " + a["name"].asString());
                                 return;
                             }
                             if (made.error().code != "branch.exists") {
                                 setStatus(made.error().message);
                                 return;
                             }
                         }
                     },
                     version > 0});
    root_.openPopup(
        ui::makeMenu("render.result.menu", std::move(items), [this] { root_.closePopup(); }),
        anchor);
}

void RenderDialog::pollJob() {
    if (job_.empty()) return;
    json::Value argsJson = json::Value::object();
    argsJson["job"] = job_;
    auto status = dispatcher_.execute({"job.get", std::move(argsJson), kUiOrigin});
    if (!status) {
        setStatus(status.error().message);
        job_.clear();
        return;
    }
    const std::string s = text(status->find("status"));
    if (s == "running") {
        const json::Value* progress = status->find("progress");
        const json::Value* fraction = progress ? progress->find("fraction") : nullptr;
        if (fraction && fraction->isNumber()) {
            char line[48];
            std::snprintf(line, sizeof line, "rendering… %3.0f%%", 100 * fraction->asDouble());
            if (mixJob_) {
                if (auto report = onStatus) report(std::string("Mixing… ") + (line + 13));
            } else
                setStatus(line);
        }
        return;
    }
    job_.clear();
    if (s == "failed") {
        const json::Value* error = status->find("error");
        const std::string message = text(error ? error->find("message") : nullptr, "render failed");
        if (mixJob_) {
            if (auto report = onStatus) report("Mix failed: " + message);
        } else
            setStatus(message);
        mixJob_ = false;
        return;
    }
    const bool cancelled = s == "cancelled" || s == "canceled";
    const json::Value* result = status->find("result");
    showResults(result && result->isObject() ? *result : json::Value::object(), cancelled);
    if (mixJob_) {
        if (auto report = onStatus)
            report(resultFiles_.empty() ? std::string("Mix: no file written")
                                        : "Mixed → " + resultFiles_.front());
    }
    mixJob_ = false;
    if (!cancelled && !resultFiles_.empty() && openFolderWhenDone && openFolderWhenDone())
        if (auto opener = openFolder)
            opener(std::filesystem::path(resultFiles_.front()).parent_path());
}

void RenderDialog::tick() {
    if (!job_.empty()) pollJob();
    if (!page_) return;
    if (dirty_ && nowMs() - lastEditMs_ >= static_cast<std::uint64_t>(debounceMs_))
        refreshPreview();
}

std::optional<std::chrono::steady_clock::time_point> RenderDialog::nextDeadline() const {
    // A running job (File → Mix, with the dialog closed too) is polled.
    if (!job_.empty()) return std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    if (!page_ || !dirty_) return {};
    return std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(lastEditMs_ + std::max(0, debounceMs_)));
}

}  // namespace supple::gui
