// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "chrome.h"
#include <supple/base/paths.h>

#include <algorithm>
#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/popup.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

// A flat menu-bar title: opens its menu on press.
class MenuTitle : public ui::Node {
  public:
    MenuTitle(std::string title, std::function<void(const ui::Rect&)> open)
        : Node("menu." + title, ui::Role::Menu), title_(std::move(title)), open_(std::move(open)) {
        interactive = true;
        layout.width = ui::SizeRule::content();
    }
    ui::Size measure(const ui::Canvas& c, ui::Size) override {
        return {c.textWidth(title_, ui::Font::Ui, c.theme().type("small")) + 20, 28};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        if (hovered()) c.fillRound(bounds().inset(2), t.radius("s"), t.color("raised"));
        c.text(bounds(), title_, ui::Font::Ui, t.type("small"), t.color("ink"),
               ui::kCenter | ui::kMiddle);
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            open_(bounds());
            return true;
        }
        return e.type == ui::PointerEvent::Type::Up;
    }
    std::string valueText() const override { return title_; }

  private:
    std::string title_;
    std::function<void(const ui::Rect&)> open_;
};

// A toolbar button that also answers a right-click (move, remove).
class ToolButton : public ui::Button {
  public:
    ToolButton(std::string id, std::string text, std::function<void()> run,
               std::function<void(const ui::Rect&)> context)
        : Button(std::move(id), std::move(text), std::move(run)), context_(std::move(context)) {}
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.button == 3 && context_) {
            if (e.type == ui::PointerEvent::Type::Down) context_(bounds());
            return true;
        }
        return Button::onPointer(e);
    }

  private:
    std::function<void(const ui::Rect&)> context_;
};

class ChromeBar : public ui::Node {
  public:
    using Node::Node;
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
    }
};

std::string shortName(const actions::Action& a) {
    const auto colon = a.name.find(": ");
    std::string s = colon == std::string::npos ? a.name : a.name.substr(colon + 2);
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

}  // namespace

ChromeLayout ChromeLayout::defaults() {
    ChromeLayout l;
    l.menus = {
        {"File",
         {"ui.file.new", "ui.file.open", "ui.file.save", "ui.file.save_as", "-",
          "ui.file.template.load", "ui.file.template.save", "-", "ui.file.import", "-",
          "ui.render.mix", "ui.render", "-", "ui.quit"}},
        {"Edit",
         {"history.undo", "history.redo", "-", "ui.actions", "ui.mouse", "-", "ui.preferences",
          "ui.audio.device"}},
        {"View",
         {"ui.actions", "ui.mixer", "ui.routing", "ui.history", "-", "ui.theme.mode",
          "ui.theme.palette", "-", "ui.screenset.save", "ui.screenset.load",
          "ui.screenset.delete"}},
        {"Track",
         {"track.add:channel", "track.add:buss", "track.add:master", "ui.track.template.load", "-",
          "ui.track.color", "ui.track.channels", "ui.fx.add", "track.duplicate:selected",
          "ui.track.template.save", "-", "track.remove:selected"}},
        {"Item",
         {"ui.midi.new", "-", "ui.item.cut", "ui.item.copy", "ui.item.paste", "ui.item.duplicate",
          "ui.item.merge", "item.split:playhead", "item.remove:selected", "-", "ui.midi.edit",
          "ui.midi.toggle"}},
        {"Transport",
         {"transport.toggle", "transport.record", "transport.loop.toggle", "-",
          "transport.seek:start", "transport.seek:end"}},
        {"Project",
         {"project.timebase:beats", "project.timebase:time", "project.timebase:beats_position"}},
        {"Help", {"ui.help.skill", "ui.help.skill_link", "ui.help.mcp", "ui.help.agent"}},
    };
    // Transport actions live in the transport bar's own row; not duplicated here.
    l.toolbar = {"history.undo", "history.redo", "track.add:channel", "ui.actions"};
    return l;
}

std::filesystem::path ChromeLayout::defaultFile(std::string_view slug) {
    if (const auto root = paths::config(); !root.empty()) return root / slug / "menus.json";
    return {};
}

Result<ChromeLayout> ChromeLayout::load(const std::filesystem::path& file) {
    ChromeLayout l = defaults();
    if (file.empty()) return l;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec) return l;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* schema = doc->isObject() ? doc->find("schema") : nullptr;
    if (!schema || !schema->isString() || schema->asString() != "supple.menus/1.0")
        return Error("menus.schema", file.string() + " is not a supple.menus/1.0 document",
                     "/schema");
    auto strings = [](const json::Value& v,
                      const std::string& path) -> Result<std::vector<std::string>> {
        if (!v.isArray()) return Error("menus.list", "expected an array of action ids", path);
        std::vector<std::string> out;
        for (const json::Value& s : v.asArray()) {
            if (!s.isString()) return Error("menus.item", "expected an action id", path);
            out.push_back(s.asString());
        }
        return out;
    };
    if (const json::Value* menus = doc->find("menus")) {
        if (!menus->isArray()) return Error("menus.list", "menus must be an array", "/menus");
        l.menus.clear();
        for (std::size_t i = 0; i < menus->asArray().size(); ++i) {
            const json::Value& m = menus->asArray()[i];
            const std::string path = "/menus/" + std::to_string(i);
            const json::Value* title = m.isObject() ? m.find("title") : nullptr;
            const json::Value* items = m.isObject() ? m.find("items") : nullptr;
            if (!title || !title->isString() || !items)
                return Error("menus.menu", "a menu is {title, items}", path);
            auto ids = strings(*items, path + "/items");
            if (!ids) return std::move(ids).error();
            l.menus.push_back({title->asString(), std::move(*ids)});
        }
    }
    if (const json::Value* toolbar = doc->find("toolbar")) {
        auto ids = strings(*toolbar, "/toolbar");
        if (!ids) return std::move(ids).error();
        l.toolbar = std::move(*ids);
    }
    // Older saved layouts must retain a route to onboarding. Preserve an
    // explicitly customized Help menu, but append it when the file predates Help.
    if (std::none_of(l.menus.begin(), l.menus.end(),
                     [](const Menu& m) { return m.title == "Help"; }))
        l.menus.push_back(defaults().menus.back());
    return l;
}

json::Value ChromeLayout::toJson() const {
    json::Value doc = json::Value::object(), menuList = json::Value::array(),
                tools = json::Value::array();
    doc["schema"] = "supple.menus/1.0";
    for (const Menu& m : menus) {
        json::Value menu = json::Value::object(), items = json::Value::array();
        menu["title"] = m.title;
        for (const std::string& id : m.items) items.push_back(id);
        menu["items"] = std::move(items);
        menuList.push_back(std::move(menu));
    }
    for (const std::string& id : toolbar) tools.push_back(id);
    doc["menus"] = std::move(menuList);
    doc["toolbar"] = std::move(tools);
    return doc;
}

Status ChromeLayout::save(const std::filesystem::path& file) const {
    if (file.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto text = json::write(toJson());
    if (!text) return std::move(text).error();
    return writeFileAtomic(file, *text + "\n");
}

namespace {

void fillChrome(ui::Node& bar, ui::Root& root, AppActions& actions, const ChromeLayout& layout,
                std::function<void(const std::string&, const ui::Rect&)> toolMenu) {
    for (const ChromeLayout::Menu& menu : layout.menus) {
        auto items = menu.items;
        bar.emplace<MenuTitle>(
            menu.title, [&root, &actions, items, title = menu.title](const ui::Rect& anchor) {
                std::vector<ui::MenuItem> entries;
                for (const std::string& id : items) {
                    if (id == "-") {
                        entries.push_back({"", "", {}, true, true});
                        continue;
                    }
                    const actions::Action* a = actions.registry().find(id);
                    if (!a) continue;  // unknown ids in a user file are skipped
                    std::string name = actions.labelFor(id);
                    if (name.empty()) name = shortName(*a) + (a->prompts.empty() ? "" : "…");
                    entries.push_back(
                        {name, actions.keysFor(id), [&actions, id] { actions.invoke(id); }});
                }
                root.openPopup(ui::makeMenu("menu." + title + ".items", std::move(entries),
                                            [&root] { root.closePopup(); }),
                               anchor);
            });
    }
    auto& gap = bar.emplace<ui::Node>("chrome.gap");
    gap.layout.width = ui::SizeRule::fixed(16);
    for (const std::string& id : layout.toolbar) {
        const actions::Action* a = actions.registry().find(id);
        if (!a) continue;
        std::function<void(const ui::Rect&)> context;
        if (toolMenu) context = [toolMenu, id](const ui::Rect& r) { toolMenu(id, r); };
        auto& b = bar.emplace<ToolButton>(
            "tool." + id, shortName(*a), [&actions, id] { actions.invoke(id); }, context);
        b.layout.height = ui::SizeRule::fixed(26);
    }
}

std::unique_ptr<ui::Node> makeBar() {
    auto bar = std::make_unique<ChromeBar>("chrome");
    bar->layout.dir = ui::Layout::Dir::Row;
    bar->layout.height = ui::SizeRule::fixed(36);
    bar->layout.padding = ui::Insets::xy(8, 4);
    bar->layout.gap = 2;
    bar->layout.align = ui::Layout::Align::Center;
    return bar;
}

}  // namespace

std::unique_ptr<ui::Node> makeChrome(ui::Root& root, AppActions& actions, ChromeLayout layout) {
    auto bar = makeBar();
    fillChrome(*bar, root, actions, layout, {});
    return bar;
}

Chrome::Chrome(ui::Root& root, AppActions& actions, ChromeLayout layout, std::filesystem::path file)
    : root_(root), actions_(actions), layout_(std::move(layout)), file_(std::move(file)) {}

std::unique_ptr<ui::Node> Chrome::build() {
    auto bar = makeBar();
    bar_ = bar.get();
    fill();
    return bar;
}

void Chrome::fill() {
    if (!bar_) return;
    bar_->clear();
    fillChrome(*bar_, root_, actions_, layout_,
               [this](const std::string& id, const ui::Rect& r) { openToolMenu(id, r); });
    bar_->relayout();
}

void Chrome::changed() {
    fill();
    if (auto saved = layout_.save(file_); !saved && onError)
        onError("menus: " + saved.error().toString());
}

bool Chrome::inToolbar(const std::string& id) const {
    return std::find(layout_.toolbar.begin(), layout_.toolbar.end(), id) != layout_.toolbar.end();
}

bool Chrome::inMenu(const std::string& title, const std::string& id) const {
    for (const ChromeLayout::Menu& m : layout_.menus)
        if (m.title == title) return std::find(m.items.begin(), m.items.end(), id) != m.items.end();
    return false;
}

void Chrome::toggleToolbar(const std::string& id) {
    if (inToolbar(id))
        std::erase(layout_.toolbar, id);
    else
        layout_.toolbar.push_back(id);
    changed();
}

void Chrome::moveTool(const std::string& id, int delta) {
    auto& t = layout_.toolbar;
    const auto at = std::find(t.begin(), t.end(), id);
    if (at == t.end()) return;
    const auto from = static_cast<int>(at - t.begin());
    const int to = std::clamp(from + delta, 0, static_cast<int>(t.size()) - 1);
    if (to == from) return;
    std::string moved = std::move(t[static_cast<std::size_t>(from)]);
    t.erase(t.begin() + from);
    t.insert(t.begin() + to, std::move(moved));
    changed();
}

void Chrome::toggleMenu(const std::string& title, const std::string& id) {
    for (ChromeLayout::Menu& m : layout_.menus) {
        if (m.title != title) continue;
        if (std::find(m.items.begin(), m.items.end(), id) != m.items.end())
            std::erase(m.items, id);
        else
            m.items.push_back(id);
        changed();
        return;
    }
}

void Chrome::reset() {
    layout_ = ChromeLayout::defaults();
    changed();
}

void Chrome::openToolMenu(const std::string& id, const ui::Rect& anchor) {
    std::vector<ui::MenuItem> items;
    ui::MenuItem left, right, remove, rule, reset;
    left.label = "Move left";
    left.run = [this, id] { moveTool(id, -1); };
    right.label = "Move right";
    right.run = [this, id] { moveTool(id, 1); };
    remove.label = "Remove from toolbar";
    remove.run = [this, id] { toggleToolbar(id); };
    rule.separator = true;
    reset.label = "Reset menus and toolbar";
    reset.run = [this] { this->reset(); };
    items = {left, right, remove, rule, reset};
    root_.openPopup(ui::makeMenu("tool.menu", std::move(items), [this] { root_.closePopup(); }),
                    {anchor.x, anchor.bottom(), 1, 1});
}

}  // namespace supple::gui
