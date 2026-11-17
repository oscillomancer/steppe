// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "crash_banner.h"
#include <utility>
#include "supple/ui/canvas.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"
namespace supple::gui {
CrashBanner::CrashBanner(std::filesystem::path directory)
    : CrashBanner(directory, [&] {
          auto notice = diagnostics::lastRunNotice(directory);
          return notice ? *notice : std::optional<diagnostics::RestartNotice>();
      }()) {}
CrashBanner::CrashBanner(std::filesystem::path directory,
                         std::optional<diagnostics::RestartNotice> notice)
    : Node("crash.notice"), directory_(std::move(directory)) {
    layout.dir = ui::Layout::Dir::Row;
    layout.width = ui::SizeRule::fraction();
    layout.height = ui::SizeRule::content();
    layout.padding = ui::Insets{16, 8, 12, 8};
    layout.gap = 12;
    layout.align = ui::Layout::Align::Center;
    show(std::move(notice));
}
void CrashBanner::show(std::optional<diagnostics::RestartNotice> notice) {
    clear();
    notice_ = std::move(notice);
    visible = notice_.has_value();
    if (!notice_) return;
    auto& text = emplace<ui::TextBlock>("crash.notice.text", diagnostics::noticeText(*notice_),
                                        ui::Font::Ui, "small", "ink");
    text.layout.width = ui::SizeRule::fraction();
    emplace<ui::Button>("crash.notice.dismiss", "Dismiss", [this] { dismiss(); });
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
    canvas.fillRect({b.x, b.y, 4.0f, b.h}, canvas.theme().color("meter-hot"));
    canvas.line({b.x, b.y + b.h - 0.5f}, {b.x + b.w, b.y + b.h - 0.5f},
                canvas.theme().color("rule"));
}
}  // namespace supple::gui
