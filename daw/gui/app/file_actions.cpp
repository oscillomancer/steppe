// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "file_actions.h"
#include <supple/base/paths.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "supple/product.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

// A dialog's answer, delivered on the UI thread through `post`.
struct DialogRequest {
    std::function<void(std::vector<std::filesystem::path>)> done;
};

void SDLCALL onDialog(void* userdata, const char* const* files, int) {
    std::unique_ptr<DialogRequest> req(static_cast<DialogRequest*>(userdata));
    std::vector<std::filesystem::path> paths;
    if (files)
        for (const char* const* f = files; *f; ++f) paths.emplace_back(*f);
    if (paths.empty()) return;  // cancelled (or an error SDL reported)
    req->done(std::move(paths));
}

const SDL_DialogFileFilter kProjectFilter[] = {{"Projects", "json"}, {"All files", "*"}};
const SDL_DialogFileFilter kAudioFilter[] = {{"Audio", "wav;flac;mp3"}, {"All files", "*"}};
const SDL_DialogFileFilter kImageFilter[] = {{"Images", "png;jpg;jpeg"}, {"All files", "*"}};
const SDL_DialogFileFilter kWavFilter[] = {{"WAV audio", "wav"}, {"All files", "*"}};
const SDL_DialogFileFilter kMidiFilter[] = {{"MIDI files", "mid;midi"}, {"All files", "*"}};

// The saved project templates: a click starts a new project from one, a right
// click offers to delete it (`remove` is true when the file is gone).
class TemplateList : public ui::List {
  public:
    TemplateList(std::string id, ui::Root& root, std::vector<std::string> names,
                 std::function<void(const std::string&)> load,
                 std::function<bool(const std::string&)> remove)
        : List(std::move(id)),
          root_(root),
          names_(std::move(names)),
          load_(std::move(load)),
          remove_(std::move(remove)) {
        rowHeight = 28;
        layout.width = ui::SizeRule::fixed(360);
        layout.height = ui::SizeRule::fixed(
            rowHeight * static_cast<float>(std::min<std::size_t>(names_.size(), 12)));
        setCount(names_.size());
        paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
            c.text({r.x + 8, r.y, r.w - 16, r.h}, names_[i], ui::Font::Ui, c.theme().type("small"),
                   c.theme().color("ink"));
        };
        onActivate = [this](std::size_t i) {
            auto callback = load_;
            const std::string name = names_[i];
            root_.closePopup();  // destroys this list: nothing after but the callback
            callback(name);
        };
        onContext = [this](std::size_t i, ui::Point at) {
            ui::MenuItem item;
            item.label = "Delete \"" + names_[i] + "\"";
            item.run = [this, i] {
                if (!remove_(names_[i])) return;
                names_.erase(names_.begin() + static_cast<std::ptrdiff_t>(i));
                select(std::nullopt);
                setCount(names_.size());
                root_.relayout();
                root_.damageAll();
                if (names_.empty()) root_.closePopup();  // destroys this list
            };
            root_.openPopup(ui::makeMenu("file.template.context", {std::move(item)},
                                         [this] { root_.closePopup(); }),
                            ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
        };
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Up && e.button == 1 && selected()) {
            if (bounds().contains(e.pos)) {
                auto activate = onActivate;
                activate(*selected());
            }
            return true;
        }
        return List::onPointer(e);
    }

  private:
    ui::Root& root_;
    std::vector<std::string> names_;
    std::function<void(const std::string&)> load_;
    std::function<bool(const std::string&)> remove_;
};

// The ids (and commands) of a kind of template's prompt and list.
std::string templateIds(FileActions::Template kind) {
    return kind == FileActions::Template::Track ? "track.template" : "file.template";
}
std::string templateCommand(FileActions::Template kind) {
    return kind == FileActions::Template::Track ? "track.template" : "project.template";
}

// The track a track template is saved from: the first selected one but a master.
std::optional<model::Id> templateTrack(const model::Session& s, const UiState& ui) {
    for (const std::string& text : ui.selectedTracks) {
        const auto id = model::Id::parse(text);
        const model::Track* t = id ? s.tracks.find(*id) : nullptr;
        if (t && t->type != model::StripType::Master) return id;
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path mediaHome() {
    const auto home = paths::home();
    if (home.empty()) return {};
    return std::filesystem::path(home) / "Documents" / (std::string(product::kSlug) + "-media");
}

DialogLifetime::~DialogLifetime() {
    std::lock_guard lock(state_->mutex);
    state_->active = false;
}

std::function<void(DialogLifetime::Paths)> DialogLifetime::guard(
    Post post, std::function<void(Paths)> done) const {
    return [weak = std::weak_ptr<State>(state_), post = std::move(post),
            done = std::move(done)](Paths paths) {
        auto state = weak.lock();
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (!state->active) return;
        post([weak, done, paths = std::move(paths)] {
            auto current = weak.lock();
            if (!current) return;
            {
                std::lock_guard check(current->mutex);
                if (!current->active) return;
            }
            done(paths);
        });
    };
}

FileActions::FileActions(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                         engine::EngineApp& app, UiState& ui, SDL_Window* window, Post post,
                         SaveDialog saveDialog)
    : root_(root),
      actions_(actions),
      dispatcher_(dispatcher),
      app_(app),
      ui_(ui),
      window_(window),
      post_(std::move(post)),
      saveDialog_(std::move(saveDialog)) {
    markSaved();
    actions_.addUi("ui.file.new", "File: new project", [this] {
        confirmDiscard("Start a new project?", [this] {
            if (exec("project.new", json::Value::object())) markSaved();
        });
    });
    actions_.addUi("ui.file.open", "File: open project…", [this] {
        confirmDiscard("Open another project?", [this] {
            auto* req = new DialogRequest{
                guardDialog([this](auto paths) { openPath(paths.front()); }, true)};
            const std::string start = dialogStart();
            SDL_ShowOpenFileDialog(onDialog, req, window_, kProjectFilter, 2,
                                   start.empty() ? nullptr : start.c_str(), false);
        });
    });
    actions_.addUi("ui.file.save", "File: save", [this] {
        if (app_.projectPath().empty()) return (void)actions_.run("ui.file.save_as");
        if (exec("project.save", json::Value::object())) markSaved();
    });
    actions_.addUi("ui.file.save_as", "File: save as…",
                   [this] { chooseSave([this](auto paths) { saveAs(paths.front()); }); });
    actions_.addUi("ui.file.template.save", "File: save template…", [this] { openSaveTemplate(); });
    actions_.addUi("ui.file.template.load", "File: load template…", [this] { openTemplateList(); });
    actions_.addUi("ui.track.template.save", "Track: save as template…",
                   [this] { openSaveTemplate(Template::Track); });
    actions_.addUi("ui.track.template.load", "Track: add from template…",
                   [this] { openTemplateList(Template::Track); });
    actions_.addUi("ui.file.import", "File: import audio…", [this] {
        auto* req = new DialogRequest{guardDialog([this](auto paths) { import(paths); })};
        SDL_ShowOpenFileDialog(onDialog, req, window_, kAudioFilter, 2, nullptr, true);
    });
}

void FileActions::registerQuit(std::function<void()> quit, bool prompt) {
    actions_.addUi("ui.quit", "File: quit", [this, quit = std::move(quit), prompt] {
        // A popup only holds interactive quits that may need a discard decision.
        const bool idle = !(app_.engine() && app_.engine()->recording()) && !dirty();
        if (!prompt || idle || !root_.popupOpen()) confirmDiscard("Quit?", quit, prompt);
    });
}

bool FileActions::currentProject(std::uint64_t generation) {
    if (app_.projectGeneration() == generation) return true;
    showError("The project changed while the dialog was open. Please try again.");
    return false;
}

std::function<void(DialogLifetime::Paths)> FileActions::guardDialog(
    std::function<void(DialogLifetime::Paths)> done, bool opening) {
    const auto generation = app_.projectGeneration();
    const auto version = app_.head().version;
    const auto branch = app_.versions().branch();
    return dialogs_.guard(
        post_, [this, generation, version, branch, opening, done = std::move(done)](auto paths) {
            if (paths.empty()) return;
            auto current = [this, generation] { return currentProject(generation); };
            if (!current()) return;
            auto complete = [current, done, paths = std::move(paths)] {
                // A replacement can also happen while the renewed prompt is open.
                if (current()) done(paths);
            };
            if (opening && (app_.head().version != version || app_.versions().branch() != branch ||
                            (app_.engine() && app_.engine()->recording())))
                confirmDiscard("Open another project?", std::move(complete));
            else
                complete();
        });
}

Result<json::Value> FileActions::exec(const std::string& command, json::Value args) {
    auto r = dispatcher_.execute({command, std::move(args), kOrigin});
    if (!r)
        showError(command + ": " + r.error().message);
    else if (command == "project.save")
        showSaveWarnings(command, *r);
    return r;
}

void FileActions::showSaveWarnings(const std::string& command, const json::Value& result) {
    // A plug-in whose state could not be read: saved, but with its old state.
    // Said in a popup: a project that reopens without it is otherwise a surprise.
    const json::Value* warnings = result.find("warnings");
    if (!warnings || !warnings->isArray() || warnings->asArray().empty()) return;
    std::string text;
    int lines = 0;
    for (const auto& w : warnings->asArray())
        if (const json::Value* m = w.find("message"); m && m->isString()) {
            text += (text.empty() ? "" : "\n") + m->asString();
            lines += 1 + static_cast<int>(m->asString().size() / 60);
        }
    showError(command + ": " + text);
    auto form = std::make_unique<ui::Node>("file.warning");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(460);
    form->layout.height = ui::SizeRule::fixed(80 + 16 * std::min(lines, 12));
    form->layout.padding = ui::Insets::all(14);
    form->layout.gap = 10;
    form->emplace<ui::Label>("file.warning.title", "Saved, but a plug-in's state could not be read",
                             ui::Font::UiBold, "body");
    form->emplace<ui::TextBlock>("file.warning.text", text, ui::Font::Ui, "small", "ink-quiet");
    auto& row = form->emplace<ui::Node>("file.warning.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(28);
    row.emplace<ui::Button>("file.warning.ok", "OK", [this] { root_.closePopup(); });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
}

void FileActions::markSaved() {
    savedVersion_ = app_.head().version;
    projectGeneration_ = app_.projectGeneration();
    savedBranch_ = app_.versions().branch();
}

bool FileActions::dirty() const {
    return app_.projectGeneration() != projectGeneration_ || app_.head().version != savedVersion_ ||
           app_.versions().branch() != savedBranch_;
}

void FileActions::showError(const std::string& message) {
    if (actions_.onError) actions_.onError(message);
}

void FileActions::openPath(const std::filesystem::path& path) {
    json::Value args = json::Value::object();
    args["path"] = path.string();
    if (exec("project.open", std::move(args))) {
        ui_.selectedTracks.clear();
        ui_.selectedItems.clear();
        markSaved();
    }
}

std::filesystem::path projectFolderPath(std::filesystem::path chosen,
                                        const std::filesystem::path& current) {
    namespace fs = std::filesystem;
    if (chosen.extension().empty()) chosen += ".json";
    const fs::path stem = chosen.stem();
    fs::path base = chosen.parent_path();
    if (base.filename() == stem) return chosen;  // already the project's own folder
    // From inside the current project's folder, a new name goes beside it.
    std::error_code ec;
    if (!current.empty() && fs::equivalent(base, current.parent_path(), ec))
        base = base.parent_path();
    return base / stem / chosen.filename();
}

void FileActions::saveAs(const std::filesystem::path& path) {
    const std::filesystem::path p = projectFolderPath(path, app_.projectPath());
    std::error_code ec;
    if (std::filesystem::exists(p, ec) && p != app_.projectPath()) {
        showError("save as: " + p.string() + " already exists; choose another name");
        return;
    }
    json::Value args = json::Value::object();
    args["path"] = p.string();
    if (exec("project.save", std::move(args))) markSaved();
}

std::string FileActions::dialogStart() const {
    if (!app_.projectPath().empty()) return app_.projectPath().string();
    const std::filesystem::path home = mediaHome();
    if (home.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(home, ec);  // a dialog cannot start in a missing folder
    return ec ? std::string() : home.string() + "/";
}

void FileActions::chooseSave(std::function<void(DialogLifetime::Paths)> done) {
    const std::string start = dialogStart();
    auto guarded = guardDialog(std::move(done));
    if (saveDialog_) return saveDialog_(start, std::move(guarded));
    auto* req = new DialogRequest{std::move(guarded)};
    SDL_ShowSaveFileDialog(onDialog, req, window_, kProjectFilter, 2,
                           start.empty() ? nullptr : start.c_str());
}

void FileActions::chooseFolder(const std::filesystem::path& start,
                               std::function<void(std::filesystem::path)> done) {
    auto* req = new DialogRequest{
        guardDialog([done = std::move(done)](auto paths) { done(paths.front()); })};
    std::error_code ec;
    std::string where;
    if (!start.empty() && std::filesystem::is_directory(start, ec)) where = start.string() + "/";
    SDL_ShowOpenFolderDialog(onDialog, req, window_, where.empty() ? nullptr : where.c_str(),
                             false);
}

void FileActions::chooseImage(const std::filesystem::path& start,
                              std::function<void(std::filesystem::path)> done) {
    auto* req = new DialogRequest{
        guardDialog([done = std::move(done)](auto paths) { done(paths.front()); })};
    std::error_code ec;
    std::string where;
    if (!start.empty() && std::filesystem::is_directory(start, ec)) where = start.string() + "/";
    SDL_ShowOpenFileDialog(onDialog, req, window_, kImageFilter, 2,
                           where.empty() ? nullptr : where.c_str(), false);
}

void FileActions::exportItem(model::Id item, std::function<void(const std::string&)> status) {
    const model::Session& s = app_.head();
    const model::Item* it = s.items.find(item);
    const model::Take* take = it && it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
    if (!take || !take->source) return status("Export: the item has no material");
    const bool midi = take->source->kind == model::Kind::Midi;
    std::string name = take->name.empty() ? item.str() : take->name;
    for (char& c : name)
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) c = '-';
    std::filesystem::path folder = app_.projectPath().empty() ? mediaHome() : app_.projectDir();
    std::error_code ec;
    if (!folder.empty()) std::filesystem::create_directories(folder, ec);
    const std::string start = (folder / (name + (midi ? ".mid" : ".wav"))).string();
    auto guarded = guardDialog([this, item, status = std::move(status)](auto paths) {
        json::Value args = json::Value::object();
        args["item"] = item.str();
        args["path"] = paths.front().string();
        auto r = dispatcher_.execute({"item.export", std::move(args), kOrigin});
        if (!r) return status("Export failed: " + r.error().message);
        status("Exported " + std::filesystem::path((*r)["path"].asString()).filename().string());
    });
    if (saveDialog_) return saveDialog_(start, std::move(guarded));
    auto* req = new DialogRequest{std::move(guarded)};
    SDL_ShowSaveFileDialog(onDialog, req, window_, midi ? kMidiFilter : kWavFilter, 2,
                           start.c_str());
}

void FileActions::openSaveTemplate(Template kind) {
    if (root_.popupOpen()) root_.closePopups();
    replaceTemplate_.clear();
    const std::string ids = templateIds(kind);
    std::string title = "Save project template";
    if (kind == Template::Track) {
        const auto track = templateTrack(app_.head(), ui_);
        if (!track) return showError("Save track template: select a channel or buss first");
        title = "Save " + model::trackLabel(app_.head(), *track) + " as a track template";
    }
    auto form = std::make_unique<ui::Node>(ids + ".save");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(420);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(14);
    form->layout.gap = 8;
    form->emplace<ui::Label>(ids + ".title", title, ui::Font::UiBold, "body");
    auto& row = form->emplace<ui::Node>(ids + ".row");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(28);
    row.layout.gap = 8;
    auto& name = row.emplace<ui::TextField>(ids + ".name", "", "Template name");
    name.layout.width = ui::SizeRule::fraction();
    name.onSubmit = [this, kind](const std::string& text) { saveTemplate(text, kind); };
    name.onCancel = [this] { root_.closePopup(); };
    row.emplace<ui::Button>(ids + ".commit", "Save",
                            [this, kind, &name] { saveTemplate(name.text(), kind); });
    row.emplace<ui::Button>(ids + ".cancel", "Cancel", [this] { root_.closePopup(); });
    form->emplace<ui::Label>(ids + ".status", "", ui::Font::Ui, "small", "ink-quiet")
        .layout.height = ui::SizeRule::fixed(18);
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    root_.focus(&name);
}

void FileActions::saveTemplate(const std::string& name, Template kind) {
    const std::string ids = templateIds(kind), command = templateCommand(kind) + ".save";
    auto* status = dynamic_cast<ui::Label*>(root_.find(ids + ".status"));
    json::Value args = json::Value::object();
    if (kind == Template::Track) {
        const auto track = templateTrack(app_.head(), ui_);
        if (!track) return showError(command + ": select a channel or buss first");
        args["track"] = track->str();
    }
    args["name"] = name;
    args["replace"] = !name.empty() && name == replaceTemplate_;
    auto saved = dispatcher_.execute({command, std::move(args), kOrigin});
    if (saved) {
        replaceTemplate_.clear();
        if (root_.find(ids + ".save")) root_.closePopup();
        if (kind == Template::Project) showSaveWarnings(command, *saved);
        return;
    }
    if (!status) return showError(command + ": " + saved.error().message);
    if (saved.error().code == "template.exists") {
        replaceTemplate_ = name;
        status->setText("\"" + name + "\" exists. Save again to replace it.");
    } else {
        status->setText(saved.error().message);
    }
    root_.damageAll();
}

void FileActions::openTemplateList(Template kind) {
    if (root_.popupOpen()) root_.closePopups();
    const bool track = kind == Template::Track;
    const std::string ids = templateIds(kind), command = templateCommand(kind);
    auto listed = exec(command + ".list", json::Value::object());
    if (!listed) return;
    std::vector<std::string> names;
    for (const json::Value& t : (*listed)["templates"].asArray())
        names.push_back(t.find("name")->asString());
    auto form = std::make_unique<ui::Node>(ids + "s");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(384);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 8;
    form->emplace<ui::Label>(ids + "s.title",
                             track ? "Add track from template" : "Load project template",
                             ui::Font::UiBold, "body");
    if (names.empty())
        form->emplace<ui::Label>(ids + "s.none",
                                 track ? "No track templates yet: Track > Save as template… "
                                         "saves the selected track."
                                       : "No templates yet: File > Save template… saves one.",
                                 ui::Font::Ui, "small", "ink-quiet");
    else
        form->emplace<TemplateList>(
            ids + ".list", root_, std::move(names),
            [this, track](const std::string& name) {
                if (track) return loadTrackTemplate(name);
                confirmDiscard("Start a new project from \"" + name + "\"?",
                               [this, name] { loadTemplate(name); });
            },
            [this, command](const std::string& name) {
                json::Value args = json::Value::object();
                args["name"] = name;
                return exec(command + ".delete", std::move(args)).ok();
            });
    if (form->find(ids + ".list"))
        form->emplace<ui::Label>(ids + "s.hint", "Right-click a template to delete it.",
                                 ui::Font::Ui, "caption", "ink-quiet");
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
}

// A track template's strip goes last under the master, selected.
void FileActions::loadTrackTemplate(const std::string& name) {
    json::Value args = json::Value::object();
    args["name"] = name;
    if (auto added = exec("track.template.load", std::move(args)))
        ui_.selectedTracks = {(*added)["track"].asString()};
}

void FileActions::loadTemplate(const std::string& name) {
    json::Value args = json::Value::object();
    args["name"] = name;
    if (exec("project.template.load", std::move(args))) {
        ui_.selectedTracks.clear();
        ui_.selectedItems.clear();
        markSaved();
    }
}

void FileActions::import(const std::vector<std::filesystem::path>& files) {
    // One file: onto the selected track (or a new one) at the playhead. Several:
    // a new track each, all at the playhead — one undo step.
    const actions::Context ctx = actions_.context();
    auto imported = dispatcher_.transact("Import audio", kOrigin, [&]() -> Result<json::Value> {
        for (std::size_t i = 0; i < files.size(); ++i) {
            std::string track;
            if (files.size() == 1 && !ui_.selectedTracks.empty()) {
                track = ui_.selectedTracks.front();
            } else {
                json::Value add = json::Value::object();
                add["name"] = files[i].stem().string();
                auto t = dispatcher_.execute({"track.add", std::move(add), kOrigin});
                if (!t) return t;
                track = (*t)["track"].asString();
            }
            json::Value item = json::Value::object();
            item["track"] = track;
            item["position"] = ctx.playheadBeats;
            item["audio_file"] = files[i].string();
            if (auto r = dispatcher_.execute({"item.add", std::move(item), kOrigin}); !r) return r;
        }
        return json::Value::object();
    });
    if (!imported) showError("import: " + imported.error().message);
}

struct FileActions::DiscardRequest {
    std::string what;
    std::function<void()> then;
    std::uint64_t generation, version;
    std::string branch;
};

bool FileActions::revalidateDiscard(const std::shared_ptr<DiscardRequest>& request) {
    // A stale request must not even stop recording in the replacement project.
    if (!currentProject(request->generation)) return false;
    const bool recording = app_.engine() && app_.engine()->recording();
    if (recording && !exec("transport.stop", json::Value::object())) return false;
    if (recording || app_.head().version != request->version ||
        app_.versions().branch() != request->branch) {
        // The old decision did not cover these takes/edits. Ask even if an
        // intervening save made this revision clean, or capture was empty.
        showDiscard(request->what, request->then);
        return false;
    }
    return true;
}

void FileActions::confirmDiscard(const std::string& what, std::function<void()> then, bool prompt) {
    // Recording is not a version until Stop commits its takes. Decide whether
    // anything would be lost only after the same command as the Stop button.
    if (app_.engine() && app_.engine()->recording() &&
        !exec("transport.stop", json::Value::object()))
        return;
    if (!prompt || !dirty()) return then();
    showDiscard(what, std::move(then));
}

void FileActions::showDiscard(const std::string& what, std::function<void()> then) {
    auto request = std::make_shared<DiscardRequest>(
        DiscardRequest{what, std::move(then), app_.projectGeneration(), app_.head().version,
                       app_.versions().branch()});
    auto form = std::make_unique<ui::Node>("file.confirm");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(420);
    form->layout.height = ui::SizeRule::fixed(130);
    form->layout.padding = ui::Insets::all(14);
    form->layout.gap = 10;
    form->emplace<ui::Label>("file.confirm.title", what, ui::Font::UiBold, "body");
    form->emplace<ui::TextBlock>("file.confirm.text", "The project has unsaved changes.",
                                 ui::Font::Ui, "small", "ink-quiet");
    auto& row = form->emplace<ui::Node>("file.confirm.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(28);
    row.layout.gap = 8;
    // Each button copies what it needs first: closePopup destroys the button.
    row.emplace<ui::Button>("file.confirm.save", "Save", [this, request] {
        auto keep = request;
        root_.closePopup();
        if (!revalidateDiscard(keep)) return;
        if (app_.projectPath().empty()) {
            // Save as first; continue after the dialog answers.
            chooseSave([this, keep](auto paths) {
                if (!revalidateDiscard(keep)) return;
                saveAs(paths.front());
                if (!dirty() && revalidateDiscard(keep)) keep->then();
            });
            return;
        }
        if (exec("project.save", json::Value::object())) {
            markSaved();
            if (revalidateDiscard(keep)) keep->then();
        }
    });
    row.emplace<ui::Button>("file.confirm.discard", "Don't save", [this, request] {
        auto keep = request;
        root_.closePopup();
        if (!revalidateDiscard(keep)) return;
        keep->then();
    });
    row.emplace<ui::Button>("file.confirm.cancel", "Cancel", [this, request] {
        const auto generation = request->generation;
        root_.closePopup();
        (void)currentProject(generation);
    });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
}

}  // namespace supple::gui
