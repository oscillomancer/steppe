// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The render dialog (docs/specs/gui.md §4.8, ADR-0055): a front end to the
// project's render settings and render.mix/stems/regions/dry-run. It opens
// on render.settings (the values that will apply, those from the user's
// default drawn quiet), edits a copy, and writes it to the project with one
// "Render settings" step only when Render or Add to queue runs and something
// changed. The preview is a live dry run (coalesced per frame); renders run
// as jobs that the dialog polls with job.get. File → Mix runs through
// mixJob() and reports in the status bar.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "supple/engine/app.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Root;
class Button;
class Label;
class TextBlock;
class TextField;
class ScrollView;
}  // namespace supple::ui

namespace supple::gui {

// A file URL for a path in generic form (forward slashes): "file:///home/a"
// on POSIX, "file:///C:/Users/a" for a drive path, "file://server/share/a"
// for a UNC path. Every byte but unreserved characters and "/" (the drive's
// colon too) is percent-encoded, UTF-8 included.
std::string fileUrl(std::string_view genericPath);

class RenderDialog {
  public:
    RenderDialog(engine::EngineApp& app, cmd::Dispatcher& dispatcher, ui::Root& root);
    ~RenderDialog();

    void open();
    void close();
    bool isOpen() const { return page_ != nullptr; }

    // Frame pass: debounced preview refresh and job polling (also while closed,
    // for File → Mix).
    void tick();
    // Last capture follows the capture: tick() runs this once a second (it
    // walks the history); a test calls it directly.
    void followCapture();
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;
    // For tests: 0 runs the preview on the next tick without a wait.
    void setDebounceMs(int ms) { debounceMs_ = std::max(0, ms); }
    void refreshPreview();  // run render.dry-run now

    enum class Source { Master, Stems, Regions };
    void setSource(Source source);
    Source source() const;
    // What a mix or stems render plays (gui.md §4.8, engine.md §3.1): the
    // arrangement; the launcher's last capture, as it was heard, on the
    // version it played on ("captured"); or the launcher as it plays now,
    // over Start and End ({from: "launcher"}). A one-off selector, never
    // written to the project or a preset; kept while the app runs, and
    // Arrangement again when the capture it chose is gone. Regions always
    // play the arrangement.
    enum class Plays { Arrangement, Capture, Launcher };
    void setPlays(Plays plays);
    Plays plays() const { return plays_; }
    // Why Last capture cannot be chosen now ("" when it can): no capture
    // with running time, or one made at another sample rate.
    std::string captureUnavailable();
    // Last capture's line from the dry run: "As heard", or "Not as heard:
    // <reasons>" in the dialog's words ("" for the other sources).
    const std::string& asHeardText() const { return asHeardText_; }
    // add_to_project: the rendered files become items on new tracks (engine).
    void setAddToProject(bool on);
    bool addToProject() const;
    // The current source's file-name pattern.
    std::string pattern() const;
    const std::string& preview() const { return previewText_; }
    const std::string& statusText() const { return statusText_; }
    // The red line: a refusal (the engine's, in the dialog's words, or a
    // field's), shown there only; "" when there is none.
    const std::string& refusal() const { return refusalText_; }
    // Start a render as a job; the id is empty when the command failed.
    const std::string& job() const { return job_; }
    void run();
    // The render queue: queue what the dialog would render now (pinned to this
    // version and these settings), show the queue, run it as one job.
    void addToQueue();
    void openQueue();
    void runQueue();
    std::vector<std::string> resultFiles() const { return resultFiles_; }
    // Edits a field as typing would (tests, scripts). Ids: render.pattern,
    // .start, .end, .perf.start, .perf.end (the launcher's range), .tail, .sample_rate, .bit_depth,
    // .format, .secondary, .channels, .normalize, .target, .limit, .mode, .tap, .handles,
    // .threshold, .meta.<member>; other ids are ignored.
    void setField(const std::string& id, const std::string& text);
    // The settings the dialog would render with (complete) and the members it
    // would write to the project (session-model §1.10 JSON).
    const json::Value& settings() const { return settings_; }
    // The arguments Render would pass (settings plus the one-off selectors:
    // version and the checked regions).
    json::Value renderArguments() const { return renderArgs(false); }
    // Regions: the grid of regions (and a Project row) by tracks and masters
    // that edits `matrix`.
    void openMatrix();
    json::Value projectSettings() const;

    // Presets (render.preset.*): load one into the dialog (not the project),
    // save the dialog's settings under a name or as the user default.
    Status loadPreset(const std::string& name);
    Status savePreset(const std::string& name);
    Status saveAsDefault();
    Status deletePreset(const std::string& name);

    // File → Mix: render.mix with no other arguments, as a job, reported by
    // onStatus.
    void mixJob();

    // The Folder row's Choose folder…: the app opens the system's folder
    // dialog and, once the renders folder changed, calls showFolder().
    std::function<void()> onChooseFolder;
    // The Cover row's Choose…: the app picks an image and calls setCover
    // with its path relative to the project folder.
    std::function<void()> onChooseCover;
    void setCover(const std::string& relative);
    // Opens a folder in the file manager (default: SDL_OpenURL).
    std::function<void(const std::filesystem::path&)> openFolder;
    // The status bar (File → Mix progress and result).
    std::function<void(const std::string&)> onStatus;
    // The app's log: a refusal the dialog has no words for, in full (code,
    // path, message and hint), while the dialog shows its plain sentence.
    std::function<void(const std::string&)> onLog;
    // An engine refusal in the dialog's words, from its code and path: never
    // its message, hint or a command name. One the dialog does not know
    // reads `fallback` and goes to onLog in full. The dialog's own errors
    // (gui.*) are already its words.
    std::string refusalWords(const Error& e,
                             std::string_view fallback = "The render could not be made.") const;
    // The "Open folder when done" preference.
    std::function<bool()> openFolderWhenDone;
    std::function<void(bool)> setOpenFolderWhenDone;
    void showFolder();  // the renders folder, as the dialog shows it
    const std::string& folderText() const { return folderText_; }

  private:
    struct Diagnostic {
        std::string field, message;
    };
    void load(const json::Value& args);  // render.settings with `args` into the dialog
    void rebuild();
    std::unique_ptr<ui::Node> buildPage();
    void buildHeader(ui::Node& page);
    void buildSource(ui::Node& body);
    void buildRange(ui::Node& body);
    void buildOutput(ui::Node& body);
    void buildFormat(ui::Node& body);
    void buildLoudness(ui::Node& body);
    void buildProcessing(ui::Node& body);
    void buildMetadata(ui::Node& body);
    void buildAfter(ui::Node& body);
    void buildFooter(ui::Node& page);
    // Edits a top-level member; the project owns it from now on.
    void set(const std::string& member, json::Value value, bool structural = false);
    json::Value& member(const std::string& name);
    json::Value& output(std::size_t index);
    bool fromDefault(const std::string& member) const;
    void numberField(const std::string& id, const std::string& text,
                     std::function<Status(double)> apply);
    void markDirty();
    void setStatus(std::string text);
    void showRefusal(std::string words);  // the red line ("" clears it)
    Status validate() const;
    json::Value selectors() const;
    json::Value renderArgs(bool forJob) const;
    std::string commandName() const;
    Status writeSettings();
    void startJob(std::string command, json::Value args);
    void pollJob();
    void showResults(const json::Value& result, bool cancelled);
    void refreshQueueButton();
    void openPresetName();
    void openExisting(const json::Value& existing);
    void chooseVersion();
    void openResultMenu(std::size_t index, const ui::Rect& anchor);
    bool clipsHeld() const;  // a track plays a clip, held or stopped: not the arrangement
    // A performance applies: Last capture or Launcher, and not Regions.
    bool performance() const;
    // The performance selector for the commands (engine.md §3.1).
    json::Value performanceArg() const;
    // The launcher's default range: from the playhead's bar for the longest
    // clip playing, in whole bars (four when none plays).
    void defaultLauncherRange();
    void buildPerformanceRange(ui::Node& body);
    // The file name a performance renders under: the one typed, or the
    // current source's pattern plus " performance".
    std::string perfPattern() const;
    // The launcher's range again from its default when it is incomplete or
    // out of order (after a Plays switch or a preset).
    void settleLauncherRange();
    // What Last capture shows of a capture: its start and version, whether it
    // runs or is kept, and once it stopped its length, launches and facts. Two
    // captures alike in all of it render alike (there is no capture serial).
    struct CaptureKey {
        std::int64_t start = 0;
        std::uint64_t version = 0;
        bool running = false, kept = false;
        std::int64_t frames = -1;  // stopped only: a running one grows
        std::size_t launches = 0;
        engine::Engine::CaptureFacts facts;  // stopped only
        bool operator==(const CaptureKey&) const = default;
    };
    std::optional<CaptureKey> captureKey() const;

    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    json::Value settings_ = json::Value::object(), initial_ = json::Value::object();
    json::Value from_ = json::Value::object();
    std::set<std::string> owned_, initialOwned_;
    std::map<std::string, std::string> raw_;     // text fields as typed
    std::map<std::string, std::string> errors_;  // field -> message
    std::size_t tab_ = 0;
    bool ask_ = false;                       // if_exists "Ask" (never written)
    std::string versionChoice_ = "current";  // "current", "branch:<name>", "version:<n>"
    bool clipsHeld_ = false;                 // what the Source section's clips line says
    Plays plays_ = Plays::Arrangement;
    std::optional<double> launcherStart_, launcherEnd_;  // beats
    // The file name typed for a performance, one-off (never written); unset,
    // the name follows the source (perfPattern()).
    std::optional<std::string> perfPattern_;
    // The capture Last capture showed at the last build (tick() follows it).
    std::optional<CaptureKey> captureKey_;
    std::string asHeardText_;
    std::optional<std::pair<double, double>> perfRange_;  // the dry run's range, beats
    // The dry run's running length in seconds, and whether it wrapped a loop.
    std::optional<std::pair<double, bool>> perfRun_;
    bool captureRunning_ = false;    // what the capture's range line says ("so far")
    std::uint64_t lastPerfMs_ = 0;   // the last dry run for a running capture
    bool captureOffered_ = false;    // Last capture could be chosen at the last build
    std::uint64_t lastOfferMs_ = 0;  // when tick() last asked
    // The checked regions when not all are (a one-off selector, never written).
    std::optional<std::vector<std::string>> regions_;
    std::string presetName_;
    std::string previewText_, statusText_, refusalText_, job_, folderText_;
    bool mixJob_ = false;  // the job is File → Mix
    std::vector<std::string> resultFiles_;
    json::Value results_ = json::Value::array();
    bool dirty_ = true;
    int debounceMs_ = 40;
    std::uint64_t lastEditMs_ = 0;

    ui::Node* page_ = nullptr;
    ui::Node* body_ = nullptr;
    ui::ScrollView* scroll_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::Label* diagnostic_ = nullptr;
    ui::Label* asHeard_ = nullptr;
    ui::TextBlock* previewBlock_ = nullptr;
    ui::TextBlock* resultsBlock_ = nullptr;
    ui::Node* resultsRow_ = nullptr;
    ui::Button* queueButton_ = nullptr;
    ui::Button* renderButton_ = nullptr;
    ui::Label* folder_ = nullptr;
    ui::TextField* patternField_ = nullptr;
    float scrollY_ = 0;
};

}  // namespace supple::gui
