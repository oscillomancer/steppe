// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The restart notice (gui.md, ADR-0083): a strip under the transport that says the last run ended
// unexpectedly, until the user dismisses it. Not modal and never in the way of recovery, which is a
// separate offer.
#pragma once
#include <filesystem>
#include <optional>
#include "supple/diagnostics/reports.h"
#include "supple/ui/node.h"
namespace supple::gui {
class CrashBanner : public ui::Node {
  public:
    // Reads the reports in `directory` once (cold, at start). Hidden when there is nothing to say.
    explicit CrashBanner(std::filesystem::path directory);
    // For tests and previews: the notice to show, whatever the folder holds.
    CrashBanner(std::filesystem::path directory, std::optional<diagnostics::RestartNotice> notice);
    void paint(ui::Canvas& canvas) override;
    // Acknowledges what the notice covered and hides the banner. Acknowledging can fail (the
    // folder is read-only); the banner goes anyway and the notice comes back next start.
    void dismiss();

  private:
    void show(std::optional<diagnostics::RestartNotice> notice);
    std::filesystem::path directory_;
    std::optional<diagnostics::RestartNotice> notice_;
};
}  // namespace supple::gui
