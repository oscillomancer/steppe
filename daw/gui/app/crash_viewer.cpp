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
constexpr float kListWidth = 280, kRowHeight = 44, kSectionLines = 40;

// A list row: when, then what. An amber bar at its left edge says nobody has marked it seen.
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
        if (selected || hovered()) c.fillRound(b, t.radius("s"), t.color("raised"));
        if (selected) c.strokeRound(b, t.radius("s"), t.color(focused() ? "focus" : "rule"));
        if (unseen_) c.fillRect({b.x, b.y + 4, 3.0f, b.h - 8}, t.color("meter-hot"));
        const float small = t.type("small");
        c.text({b.x + 12, b.y + 5, b.w - 20, small * 1.4f}, when_, ui::Font::UiBold, small,
               t.color("ink"));
        c.text({b.x + 12, b.y + 5 + small * 1.4f + 2, b.w - 20, small * 1.4f}, what_, ui::Font::Ui,
               small, t.color("ink-quiet"));
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

// Lines drawn as they are, in the mono face: indentation kept, long lines cut by an ellipsis.
class LinesBlock : public ui::Node {
  public:
    LinesBlock(std::string id, std::vector<std::string> lines)
        : Node(std::move(id), ui::Role::Label), lines_(std::move(lines)) {
        layout.height = ui::SizeRule::content();
    }
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        return {available.w, static_cast<float>(lines_.size()) * c.theme().type("caption") * 1.4f};
    }
    void paint(ui::Canvas& c) override {
        const float size = c.theme().type("caption");
        float y = bounds().y;
        for (const auto& line : lines_) {
            c.text({bounds().x, y, bounds().w, size * 1.4f}, line, ui::Font::Mono, size,
                   c.theme().color("ink-quiet"));
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

ui::Node& row(ui::Node& parent, const std::string& id, float height = 28) {
    auto& r = parent.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(height);
    r.layout.gap = 8;
    r.layout.align = ui::Layout::Align::Center;
    return r;
}

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
    const float height = std::clamp(window.h - 300.0f, 260.0f, 520.0f);
    auto form = std::make_unique<ui::Node>("crash.viewer");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(width);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(16);
    form->layout.gap = 12;
    popup_ = form.get();
    bodyHeight_ = height;
    rebuild();
    root_.openPopup(std::move(form), {}, ui::Placement::Center, [this] { popup_ = nullptr; });
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
    popup_->emplace<ui::Label>("crash.viewer.title", "Crash reports", ui::Font::UiBold, "title");
    popup_->emplace<ui::Label>(
        "crash.viewer.intro",
        "Kept on this computer only. Nothing in a report is ever sent anywhere.", ui::Font::Ui,
        "small", "ink-quiet");
    auto& body = popup_->emplace<ui::Node>("crash.viewer.body");
    body.layout.dir = ui::Layout::Dir::Row;
    body.layout.height = ui::SizeRule::fixed(bodyHeight_);
    body.layout.gap = 12;
    if (entries_.empty()) {
        body.emplace<ui::TextBlock>(
            "crash.viewer.empty",
            directory_.empty()
                ? "This copy keeps no crash reports."
                : "There are no crash reports. When an earlier run ends unexpectedly, its report "
                  "appears here.",
            ui::Font::Ui, "small", "ink-quiet");
    } else {
        auto& list = body.emplace<ui::ScrollView>("crash.viewer.list");
        list.layout.width = ui::SizeRule::fixed(kListWidth);
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
        auto& detail = body.emplace<ui::ScrollView>("crash.viewer.detail");
        detail.content().layout.gap = 10;
    }
    auto& footer = row(*popup_, "crash.viewer.footer");
    footer
        .emplace<ui::Button>("crash.viewer.folder", "Open folder",
                             [this] {
                                 if (openFolder && !directory_.empty()) openFolder(directory_);
                             })
        .quiet = true;
    auto& all = footer.emplace<ui::Button>(
        "crash.viewer.delete_all",
        deleteAllArmed_ ? "Click again to delete all " + std::to_string(entries_.size())
                        : "Delete all…",
        [this] { armDelete(true); });
    all.quiet = true;
    all.visible = !entries_.empty();
    auto& message =
        footer.emplace<ui::Label>("crash.viewer.status", "", ui::Font::Ui, "small", "ink-quiet");
    message.layout.width = ui::SizeRule::fraction();
    footer.emplace<ui::Button>("crash.viewer.close", "Close", [this] { root_.closePopup(); });
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
    std::vector<ui::TextRun> head;
    const std::size_t at =
        page_.plugin.empty() ? std::string::npos : page_.headline.find(page_.plugin);
    if (at == std::string::npos) {
        head.push_back({page_.headline, ui::Font::UiBold, "ink"});
    } else {
        if (at > 0) head.push_back({page_.headline.substr(0, at), ui::Font::Ui, "ink"});
        head.push_back({page_.plugin, ui::Font::UiBold, "ink"});
        head.push_back({page_.headline.substr(at + page_.plugin.size()), ui::Font::Ui, "ink"});
    }
    content.emplace<ui::RichText>("crash.viewer.headline", std::move(head), "body");
    content.emplace<ui::TextBlock>("crash.viewer.when", page_.subtitle, ui::Font::Ui, "small",
                                   "ink-quiet");
    // What can be done with it: the copy is the expected action, the others are quiet.
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
    actions
        .emplace<ui::Button>("crash.viewer.delete",
                             deleteArmed_ ? "Click again to delete" : "Delete",
                             [this] { armDelete(false); })
        .quiet = true;
    // Sentences: the first is what to read, the rest is how far to trust it.
    std::vector<ui::TextRun> sentences;
    for (std::size_t i = 0; i < page_.sentences.size(); ++i)
        sentences.push_back(
            {(i ? " " : "") + page_.sentences[i], ui::Font::Ui, i == 0 ? "ink" : "ink-quiet"});
    if (!sentences.empty())
        content.emplace<ui::RichText>("crash.viewer.sentences", std::move(sentences), "small");
    // The detail, folded.
    for (const auto& section : page_.sections) {
        auto& fold = content.emplace<ui::FoldSection>("crash.viewer.section." + section.id,
                                                      section.title, false);
        if (!section.note.empty())
            fold.body().emplace<Note>("crash.viewer.section." + section.id + ".note", section.note,
                                      ui::Font::Ui, "small", "ink-quiet");
        std::vector<std::string> lines = section.lines;
        if (lines.size() > kSectionLines) {
            const std::size_t more = lines.size() - static_cast<std::size_t>(kSectionLines);
            lines.resize(static_cast<std::size_t>(kSectionLines));
            lines.push_back("… " + std::to_string(more) + " more lines; Copy summary has them all");
        }
        fold.body().emplace<LinesBlock>("crash.viewer.section." + section.id + ".lines",
                                        std::move(lines));
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
