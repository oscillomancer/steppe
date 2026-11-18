// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "help_actions.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include "supple/host/clap_host.h"
#include "supple/json/write.h"
#include "supple/product.h"
#include "supple/ui/controls.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {
// Popups measure their content at the window's width; text inside the panel
// measures at the panel's inner width so it wraps as it paints.
constexpr float kPanelWidth = 720, kPanelPadding = 18,
                kPanelInner = kPanelWidth - 2 * kPanelPadding;
class Paragraph : public ui::TextBlock {
  public:
    using TextBlock::TextBlock;
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        return TextBlock::measure(c, {std::min(available.w, kPanelInner), available.h});
    }
};
std::string pathText(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
void paragraph(ui::Node& parent, const std::string& id, const std::string& text) {
    parent.emplace<Paragraph>(id, text, ui::Font::Ui, "small", "ink-quiet");
}
ui::Node& row(ui::Node& parent, const std::string& id) {
    auto& r = parent.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(30);
    r.layout.gap = 8;
    return r;
}
// JSON is code: draw explicit lines so NanoVG's word wrapper cannot discard
// indentation. Copy and display use the same string.
class JsonBlock : public ui::Node {
  public:
    explicit JsonBlock(std::string text)
        : Node("help.mcp.json", ui::Role::Label), text_(std::move(text)) {
        layout.height = ui::SizeRule::content();
    }
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        return {available.w, static_cast<float>(std::count(text_.begin(), text_.end(), '\n') + 1) *
                                 c.theme().type("caption") * 1.4f};
    }
    void paint(ui::Canvas& c) override {
        const float size = c.theme().type("caption");
        float y = bounds().y;
        std::istringstream stream(text_);
        std::string line;
        while (std::getline(stream, line)) {
            c.text({bounds().x, y, bounds().w, size * 1.4f}, line, ui::Font::Mono, size,
                   c.theme().color("ink-quiet"));
            y += size * 1.4f;
        }
    }
    std::string valueText() const override { return text_; }

  private:
    std::string text_;
};
class OutputScroll : public ui::ScrollView {
  public:
    explicit OutputScroll(std::string id) : ScrollView(std::move(id)) {
        layout.height = ui::SizeRule::content();
    }
    float maxHeight = 210;
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        const auto size = content().measure(
            c, {std::min(available.w, kPanelInner) - ui::ScrollBar::kThickness, available.h});
        return {available.w, std::min(size.h, maxHeight)};
    }
};
struct FolderRequest {
    std::function<void(DialogLifetime::Paths)> done;
    std::function<void(std::string)> error;
};
void SDLCALL folderResult(void* userdata, const char* const* files, int) {
    std::unique_ptr<FolderRequest> request(static_cast<FolderRequest*>(userdata));
    if (!files) return request->error(SDL_GetError());
    if (*files)
        request->done(
            {std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(*files)))});
}
std::string skillOutcome(const std::optional<std::filesystem::path>& source,
                         help::Harness harness) {
    if (!source)
        return "Skill not linked: the bundled skill was not found. Restore this installation's "
               "skill folder, then connect again.";
    const auto result = help::linkSkill(*source, help::skillFolders(harness));
    if (!result) return "Skill not linked: " + result.error().toString();
    const auto target = pathText(result->target);
    switch (result->outcome) {
        case help::SkillLink::Created:
            return "Skill linked at " + target + " → " + pathText(*source) +
                   ". Start a new agent session to load it.";
        case help::SkillLink::Current:
            return "Skill already installed at " + target + ".";
        case help::SkillLink::Other:
            return "A skill named " + std::string(product::kSlug) + " is already installed at " +
                   target +
                   "; left as it is. To point it at this copy, use Help > Download skill link.";
        case help::SkillLink::Occupied:
            return target +
                   " exists but is not a skill; left as it is. Move it away and connect again to "
                   "link the skill.";
    }
    return {};
}
}  // namespace
HelpActions::HelpActions(ui::Root& root, AppActions& actions, SDL_Window* window,
                         DialogLifetime::Post post, std::filesystem::path executable,
                         FolderPicker picker, std::filesystem::path diagnostics)
    : root_(root),
      window_(window),
      post_(std::move(post)),
      executables_(help::executables(executable, help::nativePlatform())),
      picker_(std::move(picker)),
      translocated_(help::isTranslocated(pathText(executable))),
      crash_(root, actions, std::move(diagnostics), window, post_) {
    actions.addUi("ui.help.skill", "Help: download skill…", [this] { download(false); });
    actions.addUi("ui.help.skill_link", "Help: download skill link…", [this] { download(true); });
    actions.addUi("ui.help.mcp", "Help: MCP server…", [this] { mcp(); });
    actions.addUi("ui.help.agent", "Help: connect an agent…", [this] { agents(); });
}
HelpActions::~HelpActions() {
    lifetime_->alive.store(false);
    lifetime_->cancelled.store(true);
    if (worker_.joinable()) worker_.join();
}
std::unique_ptr<ui::Node> HelpActions::panel(const std::string& title) {
    ++generation_;
    root_.closePopups();
    auto form = std::make_unique<ui::Node>("help.panel");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(kPanelWidth);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(kPanelPadding);
    form->layout.gap = 12;
    form->emplace<ui::Label>("help.title", title, ui::Font::UiBold, "body");
    return form;
}
void HelpActions::status(const std::string& text) {
    if (auto* label = dynamic_cast<ui::TextBlock*>(root_.find("help.status"))) {
        label->setText(text);
        label->visible = !text.empty();
        root_.relayout();
        root_.damageAll();
    }
}
void HelpActions::refreshButtons() {
    for (const auto* id : {"help.agent.claude", "help.agent.codex", "help.agent.hermes",
                           "help.agent.connect", "help.skill.choose", "help.skill.replace"})
        if (auto* button = root_.find(id)) button->enabled = !busy_;
    root_.damageAll();
}
void HelpActions::copy(const std::string& text) {
    status(SDL_SetClipboardText(text.c_str()) ? "Copied to clipboard."
                                              : "Could not copy: " + std::string(SDL_GetError()));
}
void HelpActions::background(Work work) {
    if (busy_) return status("An operation is still running. Wait for it to finish.");
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    refreshButtons();
    status("Working…");
    const unsigned generation = generation_;
    auto state = lifetime_;
    auto post = post_;
    worker_ = std::thread([this, state, post, generation, work = std::move(work)] {
        std::string result;
        try {
            result = work();
        } catch (const std::exception& error) {
            result = "Could not complete the operation: " + std::string(error.what());
        }
        if (state->alive.load())
            post([this, state, generation, result = std::move(result)] {
                if (!state->alive.load()) return;
                busy_ = false;
                refreshButtons();
                if (generation == generation_) status(result);
            });
    });
}
void HelpActions::notice(const std::string& title, const std::string& message, bool replace) {
    auto form = panel(title);
    auto& output = form->emplace<OutputScroll>("help.result.output");
    paragraph(output.content(), "help.status", message);
    auto& buttons = row(*form, "help.buttons");
    if (replace)
        buttons.emplace<ui::Button>("help.skill.replace", "Replace", [this] { install(true); });
    buttons.emplace<ui::Button>("help.close", replace ? "Cancel" : "Close",
                                [this] { root_.closePopup(); });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    refreshButtons();
}
bool HelpActions::translocationWarning(const std::string& title) {
    if (!translocated_) return false;
    const std::string name(product::kName);
    notice(title, name + " is running from a temporary location macOS assigned. Move " + name +
                      ".app to Applications (and see the install guide for clearing quarantine), "
                      "restart it, then try again.");
    return true;
}
void HelpActions::download(bool link) {
    if (link && translocationWarning("Download skill link")) return;
    if (busy_)
        return notice("Download skill", "An operation is still running. Wait for it to finish.");
    link_ = link;
    folder_.clear();
    ++generation_;
    root_.closePopups();
    chooseFolder();
}
void HelpActions::chooseFolder() {
    if (busy_) return status("An operation is still running.");
    const auto generation = generation_;
    auto done = dialogs_.guard(post_, [this, generation](auto paths) {
        if (generation == generation_) chosen(paths.front());
    });
    auto state = lifetime_;
    auto error = [this, state, post = post_, generation](std::string message) {
        if (state->alive.load())
            post([this, state, generation, message = std::move(message)] {
                if (state->alive.load() && generation == generation_)
                    notice("Download skill", "Folder picker failed: " + message +
                                                 ". Try again or choose another folder.");
            });
    };
    if (picker_) {
        picker_(
            [done](std::filesystem::path path) {
                if (!path.empty()) done({std::move(path)});
            },
            std::move(error));
        return;
    }
    SDL_ShowOpenFolderDialog(folderResult, new FolderRequest{std::move(done), std::move(error)},
                             window_, nullptr, false);
}
void HelpActions::chosen(const std::filesystem::path& parent) {
    folder_ = parent;
    const auto target = parent / std::string(product::kSlug);
    const auto kind = help::inspectTarget(target);
    if (kind == help::Target::Refuse) {
        notice("Download skill",
               "Cannot replace " + pathText(target) +
                   ": it is not a skill folder or a link. Choose another folder.");
    } else if (kind != help::Target::Missing) {
        notice("Replace existing skill?",
               pathText(target) +
                   " already exists. Replace this skill or link? Its current contents will be "
                   "removed.",
               true);
    } else {
        install(false);
    }
}

void HelpActions::install(bool replace) {
    if (folder_.empty()) return;
    const auto parent = folder_;
    const bool link = link_;
    notice(link ? "Download skill link" : "Download skill", "Working…");
    background([parent, link, replace] {
        auto source = host::skillDir();
        if (!source)
            return std::string(
                "Bundled skill not found. Restore this installation's skill folder, then try "
                "again.");
        auto result = help::exportSkill(*source, parent, link, replace);
        return result ? std::string(link ? "Link created at " : "Skill saved to ") +
                            pathText(*result) + (link ? " → " + pathText(*source) : "")
                      : "Skill download failed: " + result.error().toString();
    });
}
void HelpActions::mcp() {
    if (translocationWarning("MCP server")) return;
    auto form = panel("MCP server");
    auto& scroll = form->emplace<OutputScroll>("help.mcp.scroll");
    int windowHeight = 0;
    SDL_GetWindowSize(window_, nullptr, &windowHeight);
    const float height = root_.overlay().bounds().h > 0 ? root_.overlay().bounds().h
                                                        : static_cast<float>(windowHeight);
    scroll.maxHeight = std::min(620.0f, std::max(100.0f, height - 200.0f));
    auto& content = scroll.content();
    content.layout.gap = 12;
    const auto platform = help::nativePlatform();
    const auto proxy = help::commandText(pathText(executables_.mcp), {}, platform);
    const auto engine = help::commandText(pathText(executables_.engine), {"--mcp"}, platform);
    std::string missing;
    for (const auto& file : {executables_.mcp, executables_.engine}) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
            missing +=
                (missing.empty() ? "Missing helper: " : "\nMissing helper: ") + pathText(file);
    }
    if (!missing.empty())
        paragraph(content, "help.mcp.missing",
                  missing + "\nRestore this installation before connecting.");
    paragraph(content, "help.mcp.intro",
              "MCP clients start a stdio server by running a command. Add a stdio server named " +
                  std::string(product::kSlug) +
                  " using one of these commands (paths belong to this running copy).");
    paragraph(content, "help.mcp.proxy", "Running app\n" + proxy);
    row(content, "help.mcp.proxy.buttons")
        .emplace<ui::Button>("help.mcp.proxy.copy", "Copy running-app command",
                             [this, proxy] { copy(proxy); });
    paragraph(content, "help.mcp.headless", "Headless (renders, no window)\n" + engine);
    row(content, "help.mcp.engine.buttons")
        .emplace<ui::Button>("help.mcp.engine.copy", "Copy headless command",
                             [this, engine] { copy(engine); });
    paragraph(content, "help.mcp.instructions",
              "Start " + std::string(product::kName) +
                  " first when using the running-app server. It reconnects on the next tool call "
                  "after the app restarts. For a second instance, start the app with --instance "
                  "NAME and add --instance NAME to the server's arguments.");
    json::Value entry = json::Value::object(), servers = json::Value::object(),
                config = json::Value::object();
    entry["command"] = pathText(executables_.mcp);
    entry["args"] = json::Value::array();
    servers[std::string(product::kSlug)] = std::move(entry);
    config["mcpServers"] = std::move(servers);
    const std::string text = json::write(config).value();
    paragraph(content, "help.mcp.json.title", "Common JSON configuration");
    content.emplace<JsonBlock>(text);
    row(content, "help.mcp.json.buttons")
        .emplace<ui::Button>("help.mcp.json.copy", "Copy JSON", [this, text] { copy(text); });
    paragraph(content, "help.mcp.skill",
              "Help > Connect an agent also links the skill for Claude Code, Codex and Hermes. "
              "For other clients, use Help > Download skill or Download skill link.");
    paragraph(*form, "help.status", "");
    form->find("help.status")->visible = false;
    row(*form, "help.buttons").emplace<ui::Button>("help.close", "Close", [this] {
        root_.closePopup();
    });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    refreshButtons();
}
void HelpActions::agents() {
    if (translocationWarning("Connect an agent")) return;
    auto form = panel("Connect an agent");
    const std::string name(product::kName);
    paragraph(*form, "help.agent.intro",
              "Adds " + name +
                  "'s MCP server to your agent's settings and links the bundled skill into its "
                  "skills folder, so the agent can operate this copy of " +
                  name + ". Start " + name + " before you use the agent.");
    auto& choices = row(*form, "help.agent.choices");
    choices
        .emplace<ui::Toggle>("help.agent.claude", "Claude Code", "accent",
                             [this](bool) { select(help::Harness::Claude); })
        .layout.width = ui::SizeRule::content();
    choices
        .emplace<ui::Toggle>("help.agent.codex", "Codex", "accent",
                             [this](bool) { select(help::Harness::Codex); })
        .layout.width = ui::SizeRule::content();
    choices
        .emplace<ui::Toggle>("help.agent.hermes", "Hermes", "accent",
                             [this](bool) { select(help::Harness::Hermes); })
        .layout.width = ui::SizeRule::content();
    paragraph(*form, "help.agent.command", "");
    paragraph(*form, "help.agent.skill", "");
    auto& scroll = form->emplace<OutputScroll>("help.agent.output");
    paragraph(scroll.content(), "help.status", "");
    auto& buttons = row(*form, "help.agent.buttons");
    buttons.emplace<ui::Button>("help.agent.copy", "Copy command", [this] {
        copy(help::commandText(help::harnessCommand(harness_),
                               help::registrationArgs(harness_, pathText(executables_.mcp)),
                               help::nativePlatform()));
    });
    buttons.emplace<ui::Button>("help.agent.connect", "Connect", [this] { connect(); });
    buttons.emplace<ui::Button>("help.close", "Close", [this] { root_.closePopup(); });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    select(harness_);
    refreshButtons();
}
void HelpActions::select(help::Harness harness) {
    harness_ = harness;
    for (const auto& entry : {std::pair{"help.agent.claude", help::Harness::Claude},
                              std::pair{"help.agent.codex", help::Harness::Codex},
                              std::pair{"help.agent.hermes", help::Harness::Hermes}})
        if (auto* button = dynamic_cast<ui::Toggle*>(root_.find(entry.first)))
            button->setOn(entry.second == harness);
    if (auto* label = dynamic_cast<ui::TextBlock*>(root_.find("help.agent.command")))
        label->setText(help::commandText(
            help::harnessCommand(harness),
            help::registrationArgs(harness, pathText(executables_.mcp)), help::nativePlatform()));
    if (auto* label = dynamic_cast<ui::TextBlock*>(root_.find("help.agent.skill"))) {
        const auto folders = help::skillFolders(harness);
        label->setText(folders.empty()
                           ? std::string("No home folder was found, so Connect cannot link the "
                                         "skill. Use Help > Download skill link instead.")
                           : "Connect also links the skill as " +
                                 pathText(folders.front() / std::string(product::kSlug)) + ".");
    }
    std::string note = "Connect runs this command in the background.";
    if (harness == help::Harness::Hermes) note += " Hermes enables all discovered tools.";
    if (help::nativePlatform() == help::Platform::Windows)
        note += " The copied command is for PowerShell.";
    status(busy_ ? "An operation is still running." : note);
}
void HelpActions::connect() {
    const auto harness = harness_;
    const auto mcp = executables_.mcp;
    const auto state = lifetime_;
    background([harness, mcp, state] {
        const auto exe = help::findHarness(harness, [state] { return state->cancelled.load(); });
        if (exe.empty())
            return help::harnessCommand(harness) +
                   " was not found. Install it or copy the command and run it in your terminal.";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(mcp, ec))
            return std::string("MCP executable is missing: ") + pathText(mcp) +
                   ". Restore this installation before connecting.";
        const auto result = help::registerHarness(harness, exe, pathText(mcp),
                                                  [state] { return state->cancelled.load(); });
        // The skill is linked whatever the registration outcome; each reports separately.
        const auto skill = skillOutcome(host::skillDir(), harness);
        if (!result)
            return "Connection command failed: " + result.error().toString() + "\n" + skill;
        std::string text = result->text;
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool reportedFailure = lower.find("cancelled") != std::string::npos ||
                                     lower.find("failed to connect") != std::string::npos ||
                                     lower.find("not saved") != std::string::npos;
        std::string outcome = result->exitCode == 0 && !reportedFailure
                                  ? "Registration command completed"
                                  : "Registration command failed or was cancelled";
        if (lower.find("already exists") != std::string::npos ||
            lower.find("already configured") != std::string::npos)
            outcome =
                "A server with this name is already registered. Check its path in the agent before "
                "replacing it";
        return outcome + " (exit " + std::to_string(result->exitCode) + ").\n" + skill + "\n" +
               text;
    });
}
}  // namespace supple::gui
