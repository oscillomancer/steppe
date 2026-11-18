// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "crash_viewer.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <utility>
#include "render_dialog.h"
#include "supple/json/parse.h"
#include "supple/product.h"
#include "supple/ui/canvas.h"
#include "supple/ui/forms.h"
#include "supple/ui/popup.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace fs = std::filesystem;
namespace {
const char* const kIntro = "Kept on this computer only. Nothing in a report is ever sent anywhere.";
constexpr float kListWidth = 240, kRowHeight = 40, kSectionLines = 40;

// A list row: when, then what. Unread is the date in bold ink with a dot; seen is the date quiet.
class ReportRow : public ui::Node {
  public:
    ReportRow(std::string id, std::string when, std::string what, bool unseen,
              std::function<void()> pick, std::function<void(int)> step)
        : Node(std::move(id), ui::Role::Button),
          when_(std::move(when)),
          what_(std::move(what)),
          unseen_(unseen),
          pick_(std::move(pick)),
          step_(std::move(step)) {
        interactive = true;
        focusable = true;
        layout.height = ui::SizeRule::fixed(kRowHeight);
    }
    bool selected = false;
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        if (selected)
            c.fillRound(b, t.radius("s"), t.color("selection"));
        else if (hovered())
            c.fillRound(b, t.radius("s"), t.color("raised"));
        if (focused()) c.strokeRound(b, t.radius("s"), t.color("focus"));
        const float small = t.type("small"), caption = t.type("caption");
        const float top = b.y + 4;
        if (unseen_) c.fillCircle({b.x + 10, top + small * 0.7f}, 3.0f, t.color("accent"));
        c.text({b.x + 20, top, b.w - 28, small * 1.4f}, when_,
               unseen_ ? ui::Font::UiBold : ui::Font::Ui, small,
               t.color(unseen_ ? "ink" : "ink-quiet"));
        c.text({b.x + 20, top + small * 1.4f, b.w - 28, caption * 1.4f}, what_, ui::Font::Ui,
               caption, t.color("ink-quiet"));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.button != 1) return false;
        if (e.type == ui::PointerEvent::Type::Down) return true;
        if (e.type == ui::PointerEvent::Type::Up) {
            if (bounds().contains(e.pos) && pick_) pick_();
            return true;
        }
        return false;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_DOWN || e.key == SDLK_UP) {
            if (step_) step_(e.key == SDLK_DOWN ? 1 : -1);
            return true;
        }
        if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
            if (pick_) pick_();
            return true;
        }
        return false;
    }
    std::string valueText() const override { return when_ + " | " + what_; }

  private:
    std::string when_, what_;
    bool unseen_;
    std::function<void()> pick_;
    std::function<void(int)> step_;
};

// A scroll view as tall as its content up to `maxHeight`, measured at a width the parent cannot
// give it (a row hands its children the whole row).
class CappedScroll : public ui::ScrollView {
  public:
    using ScrollView::ScrollView;
    float maxHeight = 400, contentWidth = 300;
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        const auto size =
            content().measure(c, {contentWidth - ui::ScrollBar::kThickness, available.h});
        return {available.w, std::min(size.h, maxHeight)};
    }
};

// A section's heading: its title in bold over a rule, its count quiet after it, and Show or Hide at
// the right. The whole row takes the click.
class SectionHeading : public ui::Node {
  public:
    SectionHeading(std::string id, std::string title, std::size_t count,
                   std::function<void()> toggle)
        : Node(std::move(id), ui::Role::Button),
          title_(std::move(title)),
          count_(" (" + std::to_string(count) + ")"),
          toggle_(std::move(toggle)) {
        interactive = true;
        focusable = true;
        layout.height = ui::SizeRule::fixed(28);
    }
    bool open = false;
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        if (hovered()) c.fillRound(b, t.radius("s"), t.color("raised"));
        if (focused()) c.strokeRound(b, t.radius("s"), t.color("focus"));
        const float small = t.type("small");
        const float titleWidth = c.textWidth(title_, ui::Font::UiBold, small);
        c.text({b.x, b.y, titleWidth + 2, b.h - 1}, title_, ui::Font::UiBold, small,
               t.color("ink"));
        c.text({b.x + titleWidth, b.y, 60, b.h - 1}, count_, ui::Font::Ui, small,
               t.color("ink-quiet"));
        c.text({b.right() - 60, b.y, 56, b.h - 1}, open ? "Hide" : "Show", ui::Font::UiBold, small,
               t.color("ink-quiet"), ui::kRight | ui::kMiddle);
        c.line({b.x, b.bottom() - 0.5f}, {b.right(), b.bottom() - 0.5f}, t.color("rule"));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.button != 1) return false;
        if (e.type == ui::PointerEvent::Type::Down) return true;
        if (e.type == ui::PointerEvent::Type::Up) {
            if (bounds().contains(e.pos) && toggle_) toggle_();
            return true;
        }
        return false;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
            if (toggle_) toggle_();
            return true;
        }
        return false;
    }

  private:
    std::string title_, count_;
    std::function<void()> toggle_;
};

// A section: a heading and a body that shows when it is open.
class Section : public ui::Node {
  public:
    Section(std::string id, std::string title, std::size_t count) : Node(id) {
        layout.dir = ui::Layout::Dir::Column;
        layout.height = ui::SizeRule::content();
        layout.gap = 6;
        heading_ = &emplace<SectionHeading>(id + ".toggle", std::move(title), count,
                                            [this] { setOpen(!open_); });
        body_ = &emplace<ui::Node>(id + ".body");
        body_->layout.dir = ui::Layout::Dir::Column;
        body_->layout.height = ui::SizeRule::content();
        body_->layout.gap = 6;
        setOpen(false);
    }
    ui::Node& body() { return *body_; }
    void setOpen(bool open) {
        open_ = open;
        static_cast<SectionHeading*>(heading_)->open = open;
        heading_->invalidate();
        body_->visible = open;
        relayout();
    }
    std::string valueText() const override { return open_ ? "open" : "folded"; }

  private:
    bool open_ = false;
    ui::Node* heading_ = nullptr;
    ui::Node* body_ = nullptr;
};

// The data: lines in the mono face, in full ink, on an inset in the well colour.
class MonoInset : public ui::Node {
  public:
    MonoInset(std::string id, std::vector<std::string> lines)
        : Node(std::move(id), ui::Role::Label), lines_(std::move(lines)) {
        layout.height = ui::SizeRule::content();
    }
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        return {available.w,
                static_cast<float>(lines_.size()) * c.theme().type("caption") * 1.4f + 16};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const float size = t.type("caption");
        const ui::Rect b = bounds();
        c.fillRound(b, t.radius("s"), t.color("well"));
        float y = b.y + 8;
        for (const auto& line : lines_) {
            c.text({b.x + 8, y, std::max(0.0f, b.w - 16), size * 1.4f}, line, ui::Font::Mono, size,
                   t.color("ink"));
            y += size * 1.4f;
        }
    }
    std::string valueText() const override {
        std::string all;
        for (const auto& line : lines_) all += (all.empty() ? "" : "\n") + line;
        return all;
    }

  private:
    std::vector<std::string> lines_;
};

// A paragraph that measures a little narrower than it paints, because the sections it sits in are
// inset by their own padding, which a parent does not pass down: a line too many is room, a line
// too few would be clipped.
class Note : public ui::TextBlock {
  public:
    using TextBlock::TextBlock;
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        return TextBlock::measure(c, {std::max(1.0f, available.w - 24), available.h});
    }
};

// A vertical rule between the list and the detail.
class VerticalRule : public ui::Node {
  public:
    VerticalRule() : Node("crash.viewer.rule") { layout.width = ui::SizeRule::fixed(1); }
    void paint(ui::Canvas& c) override {
        const ui::Rect b = bounds();
        c.line({b.x + 0.5f, b.y}, {b.x + 0.5f, b.bottom()}, c.theme().color("rule"));
    }
};

ui::Node& row(ui::Node& parent, const std::string& id, float height = 28) {
    auto& r = parent.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(height);
    r.layout.gap = 8;
    r.layout.align = ui::Layout::Align::Center;
    return r;
}

struct SaveRequest {
    std::function<void(DialogLifetime::Paths)> done;
};
void SDLCALL saveAnswer(void* userdata, const char* const* files, int) {
    std::unique_ptr<SaveRequest> request(static_cast<SaveRequest*>(userdata));
    DialogLifetime::Paths paths;
    if (files)
        for (const char* const* f = files; *f; ++f)
            paths.emplace_back(std::u8string_view(reinterpret_cast<const char8_t*>(*f)));
    if (!paths.empty()) request->done(std::move(paths));  // nothing: cancelled
}
const SDL_DialogFileFilter kReportFilter[] = {{"Crash reports", "json"}, {"All files", "*"}};

std::string kindLabel(const diagnostics::ReportSummary& r) {
    std::string label;
    switch (r.kind) {
        case diagnostics::ReportKind::Crash:
            label = "Crash";
            break;
        case diagnostics::ReportKind::UnexpectedExit:
            label = "Unexpected exit";
            break;
        case diagnostics::ReportKind::BridgeExit:
            label = "Plug-in process stopped";
            break;
        case diagnostics::ReportKind::Other:
            label = "Other report";
            break;
    }
    const bool points = r.culprit && !r.culprit->name.empty() &&
                        (r.attribution == "fault_in_image" || r.attribution == "active_token" ||
                         r.attribution == "bridge_exit");
    if (points) label += " · " + r.culprit->name;
    if (r.role == "engine") label += " · headless";
    if (r.role == "fixture") label += " · test run";
    return label;
}
}  // namespace

std::string timeText(fs::file_time_type time) {
    const auto system = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                            time - fs::file_time_type::clock::now());
    const std::time_t seconds = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0) return {};
#else
    if (!localtime_r(&seconds, &local)) return {};
#endif
    char buffer[64];
    if (std::strftime(buffer, sizeof buffer, "%d %b %Y, %H:%M", &local) == 0) return {};
    std::string text = buffer;
    if (text.size() > 1 && text[0] == '0') text.erase(0, 1);
    return text;
}

CrashViewer::CrashViewer(ui::Root& root, AppActions& actions, fs::path directory,
                         SDL_Window* window, DialogLifetime::Post post)
    : root_(root), window_(window), post_(std::move(post)), directory_(std::move(directory)) {
    copyText = [](const std::string& text) { (void)SDL_SetClipboardText(text.c_str()); };
    openFolder = [](const fs::path& dir) {
        const std::string url = fileUrl(dir.generic_string());
        (void)SDL_OpenURL(url.c_str());
    };
    actions.addUi("ui.help.crash_reports", "Help: crash reports…", [this] { open(); });
}

void CrashViewer::load() {
    entries_.clear();
    if (directory_.empty()) return;
    auto listing = diagnostics::listReports(directory_);
    if (!listing) return;
    for (const auto& r : listing->reports) {
        Entry e;
        e.incident = r.incident;
        e.seen = r.acknowledged;
        e.report = r;
        e.time = r.written;
        e.when = timeText(r.written);
        entries_.push_back(std::move(e));
    }
    for (const auto& u : listing->unreported) {
        Entry e;
        e.incident = u.incident;
        e.unreported = true;
        e.seen = u.acknowledged;
        e.run = u;
        e.time = u.started;
        e.when = timeText(u.started);
        entries_.push_back(std::move(e));
    }
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return a.time > b.time; });
}

void CrashViewer::open(const std::string& incident) {
    root_.closePopups();
    load();
    selected_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].incident == incident) selected_ = i;
    deleteArmed_ = deleteAllArmed_ = false;
    const ui::Rect window = root_.overlay().bounds();
    const float width = std::clamp(window.w - 96.0f, 640.0f, 980.0f);
    const float height = std::max(200.0f, window.h - 230.0f);
    auto form = std::make_unique<ui::Node>("crash.viewer");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(width);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 16;
    popup_ = form.get();
    bodyHeight_ = height;
    popupWidth_ = width;
    rebuild();
    root_.openPopup(std::move(form), {}, ui::Placement::Center, [this] { popup_ = nullptr; });
    // A link to a report that is gone (deleted, or past what is kept) must not show another.
    if (!incident.empty() && std::none_of(entries_.begin(), entries_.end(),
                                          [&](const Entry& e) { return e.incident == incident; }))
        status("That report is no longer on this computer. These are the ones left.");
}

void CrashViewer::status(const std::string& text) {
    if (!popup_) return;
    if (auto* label = dynamic_cast<ui::Label*>(popup_->find("crash.viewer.status")))
        label->setText(text);
    root_.damageAll();
}

void CrashViewer::rebuild() {
    if (!popup_) return;
    popup_->clear();
    rows_.clear();
    auto& header = row(*popup_, "crash.viewer.header");
    header.emplace<ui::Label>("crash.viewer.title", "Crash reports", ui::Font::UiBold, "body")
        .layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("crash.viewer.close", "Close", [this] { root_.closePopup(); });
    // The line that says where reports stay is also where a message about an action shows.
    auto& line = row(*popup_, "crash.viewer.line");
    auto& message =
        line.emplace<ui::Label>("crash.viewer.status", kIntro, ui::Font::Ui, "small", "ink-quiet");
    message.layout.width = ui::SizeRule::fraction();
    line.emplace<ui::Button>("crash.viewer.folder", "Open folder",
                             [this] {
                                 if (openFolder && !directory_.empty()) openFolder(directory_);
                             })
        .quiet = true;
    auto& all = line.emplace<ui::Button>(
        "crash.viewer.delete_all",
        deleteAllArmed_ ? "Click again to delete all " + std::to_string(entries_.size())
                        : "Delete all…",
        [this] { armDelete(true); });
    all.quiet = true;
    all.visible = !entries_.empty();
    auto& body = popup_->emplace<ui::Node>("crash.viewer.body");
    body.layout.dir = ui::Layout::Dir::Row;
    body.layout.height = ui::SizeRule::content();
    body.layout.gap = 16;
    if (entries_.empty()) {
        body.emplace<ui::TextBlock>(
            "crash.viewer.empty",
            directory_.empty()
                ? "This copy keeps no crash reports."
                : "There are no crash reports. When an earlier run ends unexpectedly, its report "
                  "appears here.",
            ui::Font::Ui, "small", "ink-quiet");
    } else {
        const float padding = 24, gap = 16;
        auto& list = body.emplace<CappedScroll>("crash.viewer.list");
        list.layout.width = ui::SizeRule::fixed(kListWidth);
        list.layout.height = ui::SizeRule::content();
        list.maxHeight = bodyHeight_;
        list.contentWidth = kListWidth;
        list.content().layout.gap = 2;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            const std::string what =
                e.unreported ? std::string("No report was written") : kindLabel(e.report);
            rows_.push_back(&list.content().emplace<ReportRow>(
                "crash.viewer.row." + std::to_string(i), e.when.empty() ? "Unknown time" : e.when,
                what, !e.seen, [this, i] { select(i); },
                [this](int step) {
                    const auto next = static_cast<long long>(selected_) + step;
                    if (next >= 0 && next < static_cast<long long>(entries_.size()))
                        select(static_cast<std::size_t>(next));
                }));
        }
        body.emplace<VerticalRule>();
        auto& detail = body.emplace<CappedScroll>("crash.viewer.detail");
        detail.layout.height = ui::SizeRule::content();
        detail.maxHeight = bodyHeight_;
        detail.contentWidth = popupWidth_ - padding - kListWidth - 2 * gap - 1;
        detail.content().layout.gap = 16;
    }
    if (!entries_.empty()) select(std::min(selected_, entries_.size() - 1));
    root_.relayout();
    root_.damageAll();
}

void CrashViewer::select(std::size_t index) {
    if (index >= entries_.size()) return;
    selected_ = index;
    deleteArmed_ = false;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        static_cast<ReportRow*>(rows_[i])->selected = i == index;
        rows_[i]->invalidate();
    }
    showPage(entries_[index]);
}

void CrashViewer::showPage(const Entry& entry) {
    if (!popup_) return;
    auto* detail = dynamic_cast<ui::ScrollView*>(popup_->find("crash.viewer.detail"));
    if (!detail) return;
    // The page.
    if (entry.unreported) {
        page_ = describeUnreportedRun(entry.run.role, entry.when);
    } else {
        auto text = diagnostics::readReport(entry.incident, directory_);
        auto parsed = text ? json::parse(*text) : Result<json::Value>(text.error());
        if (parsed) {
            page_ = describeReport(*parsed, entry.when);
        } else {
            page_ = ReportPage{};
            page_.headline = "This report cannot be read.";
            page_.subtitle = entry.when.empty() ? std::string() : "Saved " + entry.when;
            page_.sentences = {
                "It may have been deleted since the list was made, or it is not a "
                "report this version can read."};
        }
    }
    auto& content = detail->content();
    content.clear();
    // The headline in full ink with a plug-in's name bold, then when and where.
    auto& top = content.emplace<ui::Node>("crash.viewer.top");
    top.layout.dir = ui::Layout::Dir::Column;
    top.layout.height = ui::SizeRule::content();
    top.layout.gap = 6;
    std::vector<ui::TextRun> head;
    const std::size_t at =
        page_.plugin.empty() ? std::string::npos : page_.headline.find(page_.plugin);
    if (at == std::string::npos) {
        head.push_back({page_.headline, ui::Font::Ui, "ink"});
    } else {
        if (at > 0) head.push_back({page_.headline.substr(0, at), ui::Font::Ui, "ink"});
        head.push_back({page_.plugin, ui::Font::UiBold, "ink"});
        head.push_back({page_.headline.substr(at + page_.plugin.size()), ui::Font::Ui, "ink"});
    }
    top.emplace<ui::RichText>("crash.viewer.headline", std::move(head), "body");
    top.emplace<ui::TextBlock>("crash.viewer.when", page_.subtitle, ui::Font::Ui, "caption",
                               "ink-quiet");
    // What can be done with it: the copy is the boxed action, the others are quiet, and Delete is
    // at the far end.
    auto& actions = row(content, "crash.viewer.actions");
    actions.emplace<ui::Button>("crash.viewer.copy", "Copy summary", [this] {
        if (copyText) copyText(page_.text());
        status("Copied to the clipboard.");
    });
    auto& save =
        actions.emplace<ui::Button>("crash.viewer.save", "Save a copy…", [this] { saveCopy(); });
    save.quiet = true;
    save.visible = !entry.unreported;
    auto& seen = actions.emplace<ui::Button>("crash.viewer.seen", "Mark as seen", [this] {
        if (selected_ >= entries_.size()) return;
        const std::string incident = entries_[selected_].incident;
        const auto result = diagnostics::acknowledge(incident, directory_);
        load();
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].incident == incident) selected_ = i;
        rebuild();
        status(result ? "Marked as seen." : "Could not mark it as seen: " + result.error().message);
    });
    seen.quiet = true;
    seen.visible = !entry.seen;
    actions.emplace<ui::Node>("crash.viewer.actions.fill").layout.width = ui::SizeRule::fraction();
    actions
        .emplace<ui::Button>("crash.viewer.delete",
                             deleteArmed_ ? "Click again to delete" : "Delete",
                             [this] { armDelete(false); })
        .quiet = true;
    // The first sentence is what to read, in ink; the rest is how far to trust it, quiet, under it.
    if (!page_.sentences.empty()) {
        auto& why = content.emplace<ui::Node>("crash.viewer.why");
        why.layout.dir = ui::Layout::Dir::Column;
        why.layout.height = ui::SizeRule::content();
        why.layout.gap = 6;
        why.emplace<ui::TextBlock>("crash.viewer.sentence", page_.sentences.front(), ui::Font::Ui,
                                   "small", "ink");
        std::string rest;
        for (std::size_t i = 1; i < page_.sentences.size(); ++i)
            rest += (rest.empty() ? "" : " ") + page_.sentences[i];
        if (!rest.empty())
            why.emplace<ui::TextBlock>("crash.viewer.sentences", rest, ui::Font::Ui, "caption",
                                       "ink-quiet");
    }
    // The detail, folded.
    for (const auto& section : page_.sections) {
        const std::string id = "crash.viewer.section." + section.id;
        auto& fold = content.emplace<Section>(id, section.title, section.count);
        if (!section.note.empty())
            fold.body().emplace<Note>(id + ".note", section.note, ui::Font::Ui, "caption",
                                      "ink-quiet");
        std::vector<std::string> lines = section.lines;
        if (lines.size() > kSectionLines) {
            const std::size_t more = lines.size() - static_cast<std::size_t>(kSectionLines);
            lines.resize(static_cast<std::size_t>(kSectionLines));
            lines.push_back("… " + std::to_string(more) + " more lines; Copy summary has them all");
        }
        if (section.id == "limits") {
            // Explanations, not data: sentences.
            std::string all;
            for (const auto& l : lines) all += (all.empty() ? "" : "\n") + l;
            fold.body().emplace<Note>(id + ".lines", all, ui::Font::Ui, "caption", "ink");
            continue;
        }
        // A line that is not indented says what the lines below it are (a thread); the indented
        // ones are frames, the data, and go on the inset.
        std::vector<std::string> block;
        std::size_t n = 0;
        const auto flush = [&] {
            if (block.empty()) return;
            fold.body().emplace<MonoInset>(id + ".lines." + std::to_string(n++), std::move(block));
            block.clear();
        };
        for (const auto& l : lines) {
            const bool frame = l.rfind("    ", 0) == 0;
            if (section.id == "threads" && !frame) {
                flush();
                fold.body().emplace<ui::Label>(id + ".head." + std::to_string(n++), l, ui::Font::Ui,
                                               "caption", "ink-quiet");
            } else {
                block.push_back(frame ? l.substr(4) : l);
            }
        }
        flush();
    }
    root_.relayout();
    root_.damageAll();
}

// The report file as it is, to a place the user picks: the only way a report leaves the folder.
// Nothing is uploaded; the copy is a file the user then does what they like with.
void CrashViewer::saveCopy() {
    if (selected_ >= entries_.size() || entries_[selected_].unreported) return;
    const std::string incident = entries_[selected_].incident;
    const std::string name =
        std::string(product::kSlug) + "-crash-" + incident.substr(0, 8) + ".json";
    const fs::path documents = mediaHome().parent_path();
    std::error_code ec;
    const fs::path start =
        !documents.empty() && fs::is_directory(documents, ec) ? documents / name : fs::path(name);
    auto answer = dialogs_.guard(post_, [this, incident](DialogLifetime::Paths paths) {
        auto text = diagnostics::readReport(incident, directory_);
        if (!text) return status("Could not save a copy: " + text.error().message);
        std::ofstream out(paths.front(), std::ios::binary | std::ios::trunc);
        out << *text;
        out.close();
        if (!out)
            return status("Could not save a copy to " + paths.front().filename().string() + ".");
        status("Saved a copy as " + paths.front().filename().string() + ".");
    });
    const auto startText = start.u8string();
    const std::string startPath(startText.begin(), startText.end());
    if (saveDialog) return saveDialog(startPath, std::move(answer));
    if (!window_ || !post_) return status("No file dialog is available here.");
    SDL_ShowSaveFileDialog(saveAnswer, new SaveRequest{std::move(answer)}, window_, kReportFilter,
                           2, startPath.c_str());
}

void CrashViewer::armDelete(bool all) {
    if (entries_.empty()) return;
    if (all ? deleteAllArmed_ : deleteArmed_) return all ? deleteAll() : doDelete();
    deleteArmed_ = !all;
    deleteAllArmed_ = all;
    if (auto* b = dynamic_cast<ui::Button*>(popup_ ? popup_->find("crash.viewer.delete") : nullptr))
        b->setText(deleteArmed_ ? "Click again to delete" : "Delete");
    if (auto* b =
            dynamic_cast<ui::Button*>(popup_ ? popup_->find("crash.viewer.delete_all") : nullptr))
        b->setText(deleteAllArmed_ ? "Click again to delete all " + std::to_string(entries_.size())
                                   : "Delete all…");
    status(all ? "This deletes every report and every record of a run without one."
               : "This deletes the report from this computer.");
}

void CrashViewer::doDelete() {
    if (selected_ >= entries_.size()) return;
    const std::string incident = entries_[selected_].incident;
    const auto result = diagnostics::deleteReport(incident, directory_);
    deleteArmed_ = deleteAllArmed_ = false;
    load();
    selected_ = std::min(selected_, entries_.empty() ? std::size_t{0} : entries_.size() - 1);
    rebuild();
    status(result ? "Deleted." : "Could not delete it: " + result.error().message);
}

void CrashViewer::deleteAll() {
    const auto result = diagnostics::deleteAllReports(directory_);
    deleteArmed_ = deleteAllArmed_ = false;
    load();
    selected_ = 0;
    rebuild();
    status(result ? "Deleted " + std::to_string(*result) + "."
                  : "Could not delete them: " + result.error().message);
}

}  // namespace supple::gui
