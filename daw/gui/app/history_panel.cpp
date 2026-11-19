// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "history_panel.h"
#include "supple/gui/process.h"

#include <ctime>

#include "supple/json/write.h"
#include "supple/ui/list.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::string clock(std::int64_t ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    const std::tm tm = localTime(t);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    return buf;
}

bool agentOrigin(const std::string& o) {
    return o.rfind("agent", 0) == 0 || o.rfind("mcp", 0) == 0;
}

}  // namespace

HistoryPanel::HistoryPanel(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                           engine::EngineApp& app)
    : root_(root), actions_(actions), dispatcher_(dispatcher), app_(app) {
    actions_.addUi("ui.history", "View: history and command log", [this] { toggle(); });
}

Result<json::Value> HistoryPanel::exec(const std::string& command, json::Value args) {
    auto r = dispatcher_.execute({command, std::move(args), kOrigin});
    if (!r && actions_.onError) actions_.onError(command + ": " + r.error().message);
    return r;
}

std::unique_ptr<ui::Node> HistoryPanel::build() {
    auto panel = std::make_unique<ui::Node>("history");
    panel_ = panel.get();
    panel->layout.dir = ui::Layout::Dir::Column;
    if (!host) panel->layout.width = ui::SizeRule::fixed(360);
    panel->layout.padding = ui::Insets::all(8);
    panel->layout.gap = 6;
    panel->visible = false;
    auto row = [](ui::Node& parent, const std::string& id) -> ui::Node& {
        auto& r = parent.emplace<ui::Node>(id);
        r.layout.dir = ui::Layout::Dir::Row;
        r.layout.height = ui::SizeRule::fixed(26);
        r.layout.gap = 6;
        r.layout.align = ui::Layout::Align::Center;
        return r;
    };
    if (!host) {  // the side panel's tab bar names and closes it otherwise
        auto& header = row(*panel, "history.header");
        auto& title =
            header.emplace<ui::Label>("history.title", "History", ui::Font::UiBold, "body");
        title.layout.width = ui::SizeRule::fraction();
        header.emplace<ui::Button>("history.close", "Close", [this] { setVisible(false); });
    }

    // Branches.
    auto& branchRow = row(*panel, "history.branchrow");
    branchLabel_ =
        &branchRow.emplace<ui::Label>("history.branch", "main", ui::Font::UiBold, "small");
    branchLabel_->layout.width = ui::SizeRule::fraction();
    branchRow.emplace<ui::Button>("history.merge", "Merge into main", [this] {
        if (current_ == "main") {
            if (actions_.onError) actions_.onError("switch to a branch to merge it into main");
            return;
        }
        json::Value args = json::Value::object();
        args["from"] = current_;
        if (exec("branch.merge", std::move(args))) (void)switchBranch("main");
    });
    branchRow.emplace<ui::Button>("history.delete", "Delete", [this] {
        if (!branchList_ || !branchList_->selected()) return;
        const std::string name = branches_[*branchList_->selected()];
        json::Value args = json::Value::object();
        args["name"] = name;
        if (exec("branch.delete", std::move(args))) reloadHistory();
    });
    branchList_ = &panel->emplace<ui::List>("history.branches");
    branchList_->rowHeight = 22;
    branchList_->layout.height = ui::SizeRule::fixed(22 * 3);
    branchList_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool selected) {
        const ui::Theme& t = c.theme();
        if (selected) c.fillRect(r, t.color("selection"));
        const bool current = branches_[i] == current_;
        c.text({r.x + 8, r.y, r.w - 16, r.h}, (current ? "• " : "  ") + branches_[i],
               current ? ui::Font::UiBold : ui::Font::Ui, t.type("small"),
               t.color(branches_[i].rfind("agent/", 0) == 0 ? "agent" : "ink"));
    };
    branchList_->onActivate = [this](std::size_t i) { (void)switchBranch(branches_[i]); };
    auto& newRow = row(*panel, "history.newrow");
    newBranch_ = &newRow.emplace<ui::TextField>("history.newname", "", "new branch name");
    newBranch_->layout.width = ui::SizeRule::fraction();
    newBranch_->onSubmit = [this](const std::string& name) { (void)createBranch(name); };
    newRow.emplace<ui::Button>("history.create", "Branch",
                               [this] { (void)createBranch(newBranch_->text()); });

    // Versions.
    auto& undoRow = row(*panel, "history.undorow");
    auto& vtitle = undoRow.emplace<ui::Label>("history.versions.title", "Versions",
                                              ui::Font::UiBold, "caption", "ink-quiet");
    vtitle.layout.width = ui::SizeRule::fraction();
    undoRow.emplace<ui::Button>("history.undo", "Undo", [this] {
        if (exec("history.undo", json::Value::object())) reloadHistory();
    });
    undoRow.emplace<ui::Button>("history.redo", "Redo", [this] {
        if (exec("history.redo", json::Value::object())) reloadHistory();
    });
    versionList_ = &panel->emplace<ui::List>("history.versions");
    versionList_->rowHeight = 22;
    versionList_->layout.height = ui::SizeRule::fraction(1);
    versionList_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r,
                                    bool selected) {
        const ui::Theme& t = c.theme();
        if (selected) c.fillRect(r, t.color("selection"));
        const Version& v = versions_[i];
        const bool agent = agentOrigin(v.origin);
        if (i == 0) c.fillRect({r.x, r.y + 3, 3, r.h - 6}, t.color("focus"));
        c.text({r.x + 8, r.y, r.w * 0.6f, r.h}, v.label.empty() ? "(edit)" : v.label, ui::Font::Ui,
               t.type("small"), t.color("ink"));
        c.text({r.x + r.w * 0.6f, r.y, r.w * 0.4f - 6, r.h}, v.origin, ui::Font::Mono,
               t.type("caption"), t.color(agent ? "agent" : "ink-faint"), ui::kRight | ui::kMiddle);
    };
    versionList_->onActivate = [this](std::size_t i) { (void)jumpTo(i); };

    // Command log.
    panel
        ->emplace<ui::Label>("history.log.title", "Commands", ui::Font::UiBold, "caption",
                             "ink-quiet")
        .layout.height = ui::SizeRule::fixed(16);
    logList_ = &panel->emplace<ui::List>("history.log");
    logList_->rowHeight = 20;
    logList_->layout.height = ui::SizeRule::fraction(1.4f);
    logList_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool selected) {
        const ui::Theme& t = c.theme();
        if (selected) c.fillRect(r, t.color("selection"));
        const Entry& e = entries_[i];
        const bool agent = agentOrigin(e.origin);
        if (agent) c.fillRect({r.x, r.y + 2, 3, r.h - 4}, t.color("agent"));
        c.text({r.x + 6, r.y, 58, r.h}, clock(e.timeMs), ui::Font::Mono, t.type("caption"),
               t.color("ink-faint"));
        c.text({r.x + 66, r.y, r.w * 0.45f, r.h}, (e.ok ? "" : "! ") + e.command, ui::Font::Ui,
               t.type("small"), t.color(e.ok ? "ink" : "clip"));
        c.text({r.x + 66 + r.w * 0.45f, r.y, r.w - 72 - r.w * 0.45f, r.h}, e.origin, ui::Font::Mono,
               t.type("caption"), t.color(agent ? "agent" : "ink-faint"), ui::kRight | ui::kMiddle);
    };
    logList_->onSelect = [this](std::size_t i) { showEntry(i); };
    detail_ = &panel->emplace<ui::TextBlock>("history.detail", "", ui::Font::Mono, "caption",
                                             "ink-quiet");
    detail_->layout.height = ui::SizeRule::content();
    return panel;
}

void HistoryPanel::toggle() {
    if (host)
        host();
    else
        setVisible(!visible());
}
bool HistoryPanel::visible() const { return panel_ && panel_->visible; }

void HistoryPanel::setVisible(bool visible) {
    if (!panel_ || panel_->visible == visible) return;
    panel_->visible = visible;
    if (!visible) root_.releaseWithin(panel_);
    if (visible) {
        version_ = ~0ull;
        sync();
    }
    root_.relayout();
    root_.damageAll();
}

void HistoryPanel::reloadHistory() {
    auto h = dispatcher_.execute({"history.list", json::Value::object(), kOrigin});
    auto b = dispatcher_.execute({"branch.list", json::Value::object(), kOrigin});
    if (!h || !b) return;
    versions_.clear();
    for (const auto& v : h->find("versions")->asArray())
        versions_.push_back({v.find("version")->asInt(), v.find("label")->asString(),
                             v.find("origin")->asString()});
    canUndo_ = h->find("can_undo")->asBool();
    canRedo_ = h->find("can_redo")->asBool();
    branches_.clear();
    for (const auto& n : b->find("branches")->asArray()) branches_.push_back(n.asString());
    current_ = b->find("current")->asString();
    if (branchLabel_) branchLabel_->setText("Branch: " + current_);
    if (versionList_) {
        versionList_->setCount(versions_.size());
        versionList_->invalidate();
    }
    if (branchList_) {
        branchList_->setCount(branches_.size());
        branchList_->invalidate();
    }
    if (auto* undo = dynamic_cast<ui::Button*>(root_.find("history.undo")))
        undo->enabled = canUndo_;
    if (auto* redo = dynamic_cast<ui::Button*>(root_.find("history.redo")))
        redo->enabled = canRedo_;
    projectGeneration_ = app_.projectGeneration();
    version_ = app_.head().version;
}

void HistoryPanel::pollLog() {
    const auto& records = app_.records();
    if (records.empty() || static_cast<std::int64_t>(records.back().seq) <= lastSeq_) return;
    json::Value args = json::Value::object();
    args["since"] = lastSeq_;
    args["limit"] = 500;
    auto r = dispatcher_.execute({"log.list", std::move(args), kOrigin});
    if (!r) return;
    const bool atEnd =
        !logList_ || !logList_->selected() || *logList_->selected() + 1 >= entries_.size();
    for (const auto& e : r->find("entries")->asArray()) {
        Entry x;
        x.seq = e.find("seq")->asInt();
        x.timeMs = e.find("time_ms")->asInt();
        x.origin = e.find("origin")->asString();
        x.command = e.find("command")->asString();
        x.args = json::writeCompact(*e.find("args"));
        x.ok = e.find("ok")->asBool();
        if (const auto* err = e.find("error"))
            x.outcome = "error: " + err->asString();
        else if (const auto* res = e.find("result"))
            x.outcome = res->asString();
        entries_.push_back(std::move(x));
    }
    if (entries_.size() > 2000)
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - 2000));
    lastSeq_ = r->find("last_seq")->asInt();
    if (logList_) {
        logList_->setCount(entries_.size());
        if (atEnd && !entries_.empty()) logList_->scrollTo(entries_.size() - 1);
        logList_->invalidate();
    }
}

void HistoryPanel::showEntry(std::size_t row) {
    if (!detail_ || row >= entries_.size()) return;
    const Entry& e = entries_[row];
    std::string args = e.args.size() > 300 ? e.args.substr(0, 300) + "…" : e.args;
    detail_->setText(e.command + "  (" + e.origin + ")\n" + args +
                     (e.outcome.empty() ? "" : "\n→ " + e.outcome));
    root_.relayout();
}

Status HistoryPanel::jumpTo(std::size_t row) {
    if (row >= versions_.size()) return Error("gui.history", "no such version");
    for (std::size_t i = 0; i < row; ++i) {
        auto r = exec("history.undo", json::Value::object());
        if (!r) return std::move(r).error();
    }
    reloadHistory();
    return {};
}

Status HistoryPanel::switchBranch(const std::string& name) {
    json::Value args = json::Value::object();
    args["name"] = name;
    auto r = exec("branch.switch", std::move(args));
    if (!r) return std::move(r).error();
    reloadHistory();
    return {};
}

Status HistoryPanel::createBranch(const std::string& name) {
    if (name.empty()) return Error("gui.history", "give the branch a name");
    json::Value args = json::Value::object();
    args["name"] = name;
    auto r = exec("branch.create", std::move(args));
    if (!r) return std::move(r).error();
    if (newBranch_) newBranch_->setText("");
    reloadHistory();
    return {};
}

void HistoryPanel::sync() {
    if (!visible()) return;
    if (app_.projectGeneration() != projectGeneration_ || app_.head().version != version_ ||
        app_.versions().branches() != branches_ || app_.versions().branch() != current_)
        reloadHistory();
    pollLog();
}

}  // namespace supple::gui
