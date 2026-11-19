// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "crash_banner.h"
#include <algorithm>
#include <utility>
#include "supple/ui/canvas.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"
namespace supple::gui {
CrashBanner::CrashBanner(std::filesystem::path directory)
    : CrashBanner(directory, [&] {
          // GUI runs only: a headless engine's incident is its own service's to announce.
          auto notice = diagnostics::lastRunNotice(directory, diagnostics::Role::Gui);
          return notice ? *notice : std::optional<diagnostics::RestartNotice>();
      }()) {}
CrashBanner::CrashBanner(std::filesystem::path directory,
                         std::optional<diagnostics::RestartNotice> notice)
    : Node("crash.notice"), directory_(std::move(directory)) {
    layout.dir = ui::Layout::Dir::Row;
    layout.width = ui::SizeRule::fraction();
    layout.height = ui::SizeRule::content();
    layout.padding = ui::Insets{12, 8, 12, 8};
    layout.gap = 12;
    layout.align = ui::Layout::Align::Center;
    show(std::move(notice));
}

void CrashBanner::setOnShowReport(std::function<void(const std::string&)> fn) {
    onShowReport_ = std::move(fn);
    show(notice_);
    relayout();
    invalidate();
}

void CrashBanner::show(std::optional<diagnostics::RestartNotice> notice) {
    clear();
    text_ = buttons_ = nullptr;
    notice_ = std::move(notice);
    visible = notice_.has_value();
    if (!notice_) return;
    // The first sentence in full ink with the plug-in's name in bold, what follows quiet.
    std::vector<ui::TextRun> runs;
    const std::string& head = notice_->headline;
    const std::size_t at = notice_->plugin.empty() ? std::string::npos : head.find(notice_->plugin);
    if (at == std::string::npos) {
        runs.push_back({head, ui::Font::Ui, "ink"});
    } else {
        if (at > 0) runs.push_back({head.substr(0, at), ui::Font::Ui, "ink"});
        runs.push_back({notice_->plugin, ui::Font::UiBold, "ink"});
        runs.push_back({head.substr(at + notice_->plugin.size()), ui::Font::Ui, "ink"});
    }
    // What follows the first sentence is short: a report's own sentences are the viewer's.
    const std::string rest = notice_->kind == diagnostics::RestartNotice::Kind::Report
                                 ? std::string("A report was saved on this computer.")
                                 : notice_->detail;
    if (!rest.empty()) runs.push_back({" " + rest, ui::Font::Ui, "ink-quiet"});
    auto& text = emplace<ui::RichText>("crash.notice.text", std::move(runs), "small");
    text.layout.width = ui::SizeRule::fraction();
    text_ = &text;
    auto& buttons = emplace<ui::Node>("crash.notice.buttons");
    buttons.layout.dir = ui::Layout::Dir::Row;
    buttons.layout.width = ui::SizeRule::content();
    buttons.layout.height = ui::SizeRule::content();
    buttons.layout.gap = 8;
    buttons_ = &buttons;
    buttons.layout.align = ui::Layout::Align::Center;
    const bool reportShown =
        onShowReport_ && notice_->kind == diagnostics::RestartNotice::Kind::Report;
    if (reportShown) {
        const std::size_t count = notice_->incidents.size();
        buttons.emplace<ui::Button>(
            "crash.notice.show",
            count > 1 ? "Show reports (" + std::to_string(count) + ")" : "Show report", [this] {
                if (onShowReport_ && notice_ && !notice_->incidents.empty())
                    onShowReport_(notice_->incidents.front());
            });
    }
    // One boxed button for the action expected: Dismiss when it is the only one.
    auto& dismiss =
        buttons.emplace<ui::Button>("crash.notice.dismiss", "Dismiss", [this] { this->dismiss(); });
    dismiss.quiet = reportShown;
}

float CrashBanner::fitText(const ui::Canvas& canvas, float width) {
    if (!text_ || !buttons_) return 0;
    const ui::Size buttons = buttons_->measure(canvas, {width, 1000});
    const float room =
        std::max(1.0f, width - layout.padding.left - layout.padding.right - buttons.w - layout.gap);
    text_->layout.height = ui::SizeRule::content();
    const ui::Size lines = text_->measure(canvas, {room, 1000});
    text_->layout.width = ui::SizeRule::fixed(room);
    text_->layout.height = ui::SizeRule::fixed(lines.h);
    return std::max(lines.h, buttons.h);
}

ui::Size CrashBanner::measure(const ui::Canvas& canvas, ui::Size available) {
    const float content = fitText(canvas, available.w);
    return {available.w, content + layout.padding.top + layout.padding.bottom};
}

void CrashBanner::arrange(const ui::Canvas& canvas, const ui::Rect& bounds) {
    fitText(canvas, bounds.w);
    Node::arrange(canvas, bounds);
}

void CrashBanner::dismiss() {
    if (!notice_) return;
    (void)diagnostics::acknowledgeNotice(*notice_, directory_);
    visible = false;
    notice_.reset();
    relayout();
    invalidate();
}
void CrashBanner::paint(ui::Canvas& canvas) {
    const ui::Rect b = bounds();
    canvas.fillRect(b, canvas.theme().color("raised"));
    canvas.fillRect({b.x, b.y, 3.0f, b.h}, canvas.theme().color("meter-hot"));
    canvas.line({b.x, b.y + b.h - 0.5f}, {b.x + b.w, b.y + b.h - 0.5f},
                canvas.theme().color("rule"));
}
}  // namespace supple::gui
