// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The restart notice (gui.md, ADR-0083): a strip under the transport that says the last run ended
// unexpectedly, until the user dismisses it. Not modal and never in the way of recovery, which is a
// separate offer.
#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include "supple/diagnostics/reports.h"
#include "supple/ui/node.h"
namespace supple::gui {
class CrashBanner : public ui::Node {
  public:
    // Reads the reports in `directory` once (cold, at start). Hidden when there is nothing to say.
    explicit CrashBanner(std::filesystem::path directory);
    // For tests and previews: the notice to show, whatever the folder holds.
    CrashBanner(std::filesystem::path directory, std::optional<diagnostics::RestartNotice> notice);
    // The text takes the width the buttons leave, and the band is as tall as its lines: a row
    // measures a wrapped paragraph at its own full width otherwise.
    ui::Size measure(const ui::Canvas& canvas, ui::Size available) override;
    void arrange(const ui::Canvas& canvas, const ui::Rect& bounds) override;
    void paint(ui::Canvas& canvas) override;
    // "Show report" opens the viewer at the newest incident the notice covers. Without it (or
    // when no report was written) the banner has Dismiss alone. The button counts the incidents
    // the notice covers: "Show report", or "Show reports (3)".
    void setOnShowReport(std::function<void(const std::string& incident)> fn);
    // Acknowledges what the notice covered and hides the banner. Acknowledging can fail (the
    // folder is read-only); the banner goes anyway and the notice comes back next start.
    void dismiss();

  private:
    float fitText(const ui::Canvas& canvas, float width);  // returns the band's content height
    void show(std::optional<diagnostics::RestartNotice> notice);
    std::filesystem::path directory_;
    std::optional<diagnostics::RestartNotice> notice_;
    std::function<void(const std::string&)> onShowReport_;
    ui::Node* text_ = nullptr;
    ui::Node* buttons_ = nullptr;
};
}  // namespace supple::gui
