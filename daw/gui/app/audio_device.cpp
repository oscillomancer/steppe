// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "audio_device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "supple/ui/popup.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

constexpr std::uint32_t kMaxInputs = 32;  // as many as JACK opens
constexpr unsigned kBlocks[] = {32, 64, 128, 256, 512, 1024, 2048};
constexpr double kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

Error audioError(const std::string& field, const std::string& message, const std::string& hint) {
    return Error("preferences.audio", message, "/audio/" + field, hint);
}

Status validate(const AudioDeviceSettings& s) {
    if (!s.backend.empty() && s.backend != "jack" && s.backend != "portaudio" &&
        s.backend != "dummy")
        return audioError("backend", "backend is jack, portaudio or dummy",
                          "choose the audio system again, or remove the field for the default");
    if (!std::isfinite(s.rate) || s.rate < 0 || s.rate > 768000)
        return audioError("rate", "rate is out of range",
                          "use 0 for the device's own rate or a rate from 1 to 768000 Hz");
    if (s.block < 16 || s.block > 4096)
        return audioError("block", "block is out of range", "use a buffer size from 16 to 4096");
    return {};
}

const engine::AudioDevice* findDevice(std::span<const engine::AudioDevice> devices,
                                      const std::string& host, const std::string& name,
                                      bool input) {
    for (const auto& d : devices) {
        if (d.hostApi != host || (input ? d.inputs : d.outputs) == 0) continue;
        if (name.empty() ? (input ? d.defaultInput : d.defaultOutput) : d.name == name) return &d;
    }
    return nullptr;
}

// An unset host API: WASAPI where it lists devices (shared mode plays beside
// other apps; PortAudio lists ASIO first, which holds the driver), else the
// first listed, PortAudio's default.
std::string resolvedHost(const AudioDeviceSettings& s,
                         std::span<const engine::AudioDevice> devices) {
    if (!s.hostApi.empty()) return s.hostApi;
    for (const auto& d : devices)
        if (d.hostApi == "Windows WASAPI") return d.hostApi;
    return devices.empty() ? std::string() : devices.front().hostApi;
}

bool isAsio(const std::string& host) { return host == "ASIO"; }

std::string hz(double rate) {
    char text[32];
    std::snprintf(text, sizeof text, "%.0f Hz", rate);
    return text;
}

std::string frames(unsigned block, double rate) {
    char text[48];
    std::snprintf(text, sizeof text, "%u frames (%.1f ms)", block, 1000.0 * block / rate);
    return text;
}

std::string message(const Error& e) {
    return e.hint.empty() ? e.message : e.message + "; " + e.hint;
}

ui::Node& row(ui::Node& form, const std::string& id, const std::string& label) {
    auto& r = form.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(26);
    r.layout.gap = 8;
    r.layout.align = ui::Layout::Align::Center;
    r.emplace<ui::Label>(id + ".label", label, ui::Font::Ui, "small", "ink-quiet").layout.width =
        ui::SizeRule::fixed(160);
    return r;
}

ui::Label& note(ui::Node& form, const std::string& id, const std::string& text) {
    auto& label = form.emplace<ui::Label>(id, text, ui::Font::Ui, "small", "ink-quiet");
    label.layout.width = ui::SizeRule::fraction();
    label.layout.height = ui::SizeRule::fixed(18);
    return label;
}

}  // namespace

std::string defaultAudioBackend() {
#if defined(__linux__)
    return "jack";
#else
    return "portaudio";
#endif
}

std::string effectiveBackend(const AudioDeviceSettings& settings) {
    return settings.backend.empty() ? defaultAudioBackend() : settings.backend;
}

Result<AudioDeviceSettings> parseAudioDevice(const json::Value& value) {
    if (!value.isObject())
        return audioError("", "audio must be an object", "remove the field to use the defaults");
    AudioDeviceSettings s;
    auto text = [&](const char* field, std::string& out) -> Status {
        if (const auto* v = value.find(field)) {
            if (!v->isString())
                return audioError(field, std::string(field) + " must be a string",
                                  "choose it again in Audio device, or remove the field");
            out = v->asString();
        }
        return {};
    };
    auto flag = [&](const char* field, bool& out) -> Status {
        if (const auto* v = value.find(field)) {
            if (!v->isBool())
                return audioError(field, std::string(field) + " must be a boolean",
                                  "use true or false");
            out = v->asBool();
        }
        return {};
    };
    SUPPLE_TRY(text("backend", s.backend));
    SUPPLE_TRY(text("host_api", s.hostApi));
    SUPPLE_TRY(text("output_device", s.outputDevice));
    SUPPLE_TRY(text("input_device", s.inputDevice));
    SUPPLE_TRY(flag("record", s.record));
    SUPPLE_TRY(flag("force_graph", s.forceGraph));
    if (const auto* v = value.find("rate")) {
        if (!v->isNumber())
            return audioError("rate", "rate must be a number",
                              "use 0 for the device's own rate or a rate in Hz");
        s.rate = v->isInt() ? static_cast<double>(v->asInt()) : v->asDouble();
    }
    if (const auto* v = value.find("block")) {
        if (!v->isInt() || v->asInt() < 16 || v->asInt() > 4096)
            return audioError("block", "block must be an integer from 16 to 4096",
                              "use a buffer size from 16 to 4096");
        s.block = static_cast<unsigned>(v->asInt());
    }
    SUPPLE_TRY(validate(s));
    return s;
}

json::Value audioDeviceJson(const AudioDeviceSettings& s) {
    json::Value v = json::Value::object();
    if (!s.backend.empty()) v["backend"] = s.backend;
    if (!s.hostApi.empty()) v["host_api"] = s.hostApi;
    if (!s.outputDevice.empty()) v["output_device"] = s.outputDevice;
    if (!s.inputDevice.empty()) v["input_device"] = s.inputDevice;
    v["record"] = s.record;
    if (s.rate == std::floor(s.rate))
        v["rate"] = static_cast<std::int64_t>(s.rate);
    else
        v["rate"] = s.rate;
    v["block"] = static_cast<std::int64_t>(s.block);
    v["force_graph"] = s.forceGraph;
    return v;
}

json::Value engineStartArgs(const AudioDeviceSettings& s,
                            std::span<const engine::AudioDevice> devices) {
    json::Value args = json::Value::object();
    const std::string backend = effectiveBackend(s);
    args["backend"] = backend;
    args["block"] = static_cast<std::int64_t>(s.block);
    double rate = s.rate;
    if (backend == "portaudio") {
        const std::string host = resolvedHost(s, devices);
        if (!host.empty()) args["host_api"] = host;
        if (!s.outputDevice.empty()) args["output_device"] = s.outputDevice;
        const auto* output = findDevice(devices, host, s.outputDevice, false);
        // Automatic: the output device's own rate, so nothing converts.
        if (rate <= 0 && output && output->defaultRate > 0) rate = output->defaultRate;
        if (s.record) {
            // ASIO opens one device for both directions.
            const std::string inputName =
                isAsio(host) ? (output ? output->name : s.outputDevice) : s.inputDevice;
            if (const auto* input = findDevice(devices, host, inputName, true)) {
                if (!inputName.empty()) args["input_device"] = inputName;
                args["inputs"] = static_cast<std::int64_t>(std::min(input->inputs, kMaxInputs));
            } else if (!inputName.empty()) {
                // Not listed now: engine.start names what is missing.
                args["input_device"] = inputName;
                args["inputs"] = std::int64_t{2};
            }
        }
        // Every output the device has (up to 64), so any master can play to
        // any of them (ADR-0056).
        if (output && output->outputs > 2)
            args["outputs"] =
                static_cast<std::int64_t>(std::min<std::uint32_t>(output->outputs, 64));
    } else if (backend == "jack" && s.forceGraph) {
        args["force_graph"] = true;
    }
    if (rate > 0) args["rate"] = rate;
    return args;
}

AudioDeviceDialog::AudioDeviceDialog(ui::Root& root, cmd::Dispatcher& dispatcher,
                                     engine::EngineApp& app, AudioDeviceSettings settings,
                                     Save save)
    : root_(root),
      dispatcher_(dispatcher),
      app_(app),
      settings_(std::move(settings)),
      save_(std::move(save)) {
    enumerate = [this]() -> Result<std::vector<engine::AudioDevice>> {
        json::Value args = json::Value::object();
        args["backend"] = "portaudio";
        auto listed = dispatcher_.execute({"engine.devices", std::move(args), kOrigin});
        if (!listed) return std::move(listed).error();
        std::vector<engine::AudioDevice> out;
        if (const auto* list = listed->find("devices"); list && list->isArray())
            for (const auto& d : list->asArray()) {
                engine::AudioDevice device;
                device.hostApi = d.find("host_api")->asString();
                device.name = d.find("name")->asString();
                device.inputs = static_cast<std::uint32_t>(d.find("inputs")->asInt());
                device.outputs = static_cast<std::uint32_t>(d.find("outputs")->asInt());
                const auto* rate = d.find("default_rate");
                device.defaultRate = rate->isInt() ? double(rate->asInt()) : rate->asDouble();
                device.defaultInput = d.find("default_input")->asBool();
                device.defaultOutput = d.find("default_output")->asBool();
                out.push_back(std::move(device));
            }
        return out;
    };
    startEngine = [this](const json::Value& args) {
        return dispatcher_.execute({"engine.start", args, kOrigin});
    };
    stopEngine = [this] {
        (void)dispatcher_.execute({"engine.stop", json::Value::object(), kOrigin});
    };
}

std::vector<engine::AudioDevice> AudioDeviceDialog::devices(bool reload) {
    if (reload || (devices_.empty() && enumerateError_.empty())) {
        auto listed = enumerate();
        devices_ = listed ? std::move(*listed) : std::vector<engine::AudioDevice>{};
        enumerateError_ = listed ? std::string() : message(listed.error());
    }
    return devices_;
}

std::string AudioDeviceDialog::hostApiName() const { return resolvedHost(settings_, devices_); }

Status AudioDeviceDialog::start(bool fallback) {
    wanted_ = true;
    auto attempt = [&](const AudioDeviceSettings& s) -> Status {
        const auto listed =
            effectiveBackend(s) == "portaudio" ? devices(true) : std::vector<engine::AudioDevice>{};
        auto started = startEngine(engineStartArgs(s, listed));
        if (!started) return std::move(started).error();
        running_ = s;
        return {};
    };
    Status st = attempt(settings_);
    if (!st && fallback) {
        AudioDeviceSettings defaults;
        defaults.backend = settings_.backend;
        defaults.block = settings_.block;
        if (!(defaults == settings_) && attempt(defaults)) {
            lastError_ = message(st.error()) + " (using the default device)";
            if (onError) onError("audio device: " + lastError_);
            refresh();
            return {};
        }
    }
    lastError_ = st ? std::string() : message(st.error());
    refresh();
    return st;
}

Status AudioDeviceDialog::restart() {
    if (app_.backend()) stopEngine();
    return start(false);
}

void AudioDeviceDialog::stop() {
    wanted_ = false;
    lastError_.clear();
    stopEngine();
    refresh();
}

Status AudioDeviceDialog::apply(AudioDeviceSettings next) {
    SUPPLE_TRY(validate(next));
    if (save_) SUPPLE_TRY(save_(next));
    settings_ = std::move(next);
    Status st;
    if (wanted_ || app_.backend()) st = restart();
    refresh();
    return st;
}

void AudioDeviceDialog::choose(AudioDeviceSettings next) {
    // The status lines show a failure too: stderr is not seen from Finder.
    if (auto st = apply(std::move(next)); !st) {
        lastError_ = message(st.error());
        if (onError) onError("audio device: " + lastError_);
    }
    if (isOpen()) build(false);  // rows depend on the audio system
}

std::string AudioDeviceDialog::summary() const {
    const std::string backend = effectiveBackend(settings_);
    std::string out = backend == "jack"           ? "JACK (PipeWire)"
                      : backend == "dummy"        ? "Dummy (no device)"
                      : settings_.hostApi.empty() ? "Native device"
                                                  : settings_.hostApi;
    if (backend == "portaudio")
        out += " · " + (settings_.outputDevice.empty() ? std::string("System default")
                                                       : settings_.outputDevice);
    out += " · " + (settings_.rate > 0 ? hz(settings_.rate) : std::string("own rate"));
    out += " · " + std::to_string(settings_.block) + " frames";
    return out;
}

bool AudioDeviceDialog::isOpen() const { return root_.find("audio") != nullptr; }

void AudioDeviceDialog::openMenu(const std::string& id, std::vector<ui::MenuItem> items) {
    const ui::Node* anchor = root_.find(id + ".set");
    root_.openPopup(ui::makeMenu(id + ".menu", std::move(items), [this] { root_.closePopup(); }),
                    anchor ? anchor->bounds() : ui::Rect{});
}

void AudioDeviceDialog::open() { build(true); }

void AudioDeviceDialog::build(bool reload) {
    if (root_.popupOpen()) root_.closePopups();
    const std::string backend = effectiveBackend(settings_);
    if (backend == "portaudio") devices(reload);
    const std::string host = hostApiName();
    auto form = std::make_unique<ui::Node>("audio");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(600);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 6;
    auto& header = form->emplace<ui::Node>("audio.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(28);
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("audio.title", "Audio device", ui::Font::UiBold, "body")
        .layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("audio.close", "Close", [this] { root_.closePopups(); });

    auto button = [&](const std::string& id, const std::string& label, const std::string& text,
                      std::function<void()> click) {
        auto& r = row(*form, id, label);
        auto& b = r.emplace<ui::Button>(id + ".set", text, std::move(click));
        b.layout.width = ui::SizeRule::fraction();
        b.layout.height = ui::SizeRule::fixed(26);
    };

    // The audio system: JACK on Linux, each native host API elsewhere, or none.
    std::string systemText = backend == "jack"    ? "JACK (PipeWire)"
                             : backend == "dummy" ? "Dummy (no device)"
                             : host.empty()       ? "Native device"
                                                  : host;
    button("audio.system", "Audio system", systemText, [this] {
        std::vector<ui::MenuItem> items;
        const std::string current = effectiveBackend(settings_);
        auto add = [&](std::string label, bool checked, AudioDeviceSettings next) {
            ui::MenuItem item;
            item.label = std::move(label);
            item.checked = checked;
            item.run = [this, next] { choose(next); };
            items.push_back(std::move(item));
        };
#if defined(__linux__)
        {
            AudioDeviceSettings next = settings_;
            next.backend = "jack";
            add("JACK (PipeWire)", current == "jack", next);
        }
#else
        std::vector<std::string> hosts;
        for (const auto& d : devices(true))
            if (std::find(hosts.begin(), hosts.end(), d.hostApi) == hosts.end())
                hosts.push_back(d.hostApi);
        for (const auto& h : hosts) {
            AudioDeviceSettings next = settings_;
            next.backend = "portaudio";
            if (next.hostApi != h) next.outputDevice = next.inputDevice = {};
            next.hostApi = h;
            add(h, current == "portaudio" && hostApiName() == h, next);
        }
        if (hosts.empty()) {
            ui::MenuItem item;
            item.label = "Native device: " +
                         (enumerateError_.empty() ? std::string("none found") : enumerateError_);
            item.enabled = false;
            items.push_back(std::move(item));
        }
#endif
        AudioDeviceSettings dummy = settings_;
        dummy.backend = "dummy";
        add("Dummy (no device)", current == "dummy", dummy);
        openMenu("audio.system", std::move(items));
    });

    if (backend == "portaudio") {
        button("audio.output", "Output device",
               settings_.outputDevice.empty() ? "System default" : settings_.outputDevice, [this] {
                   std::vector<ui::MenuItem> items;
                   auto add = [&](std::string label, const std::string& name) {
                       ui::MenuItem item;
                       item.label = std::move(label);
                       item.checked = settings_.outputDevice == name;
                       item.run = [this, name] {
                           AudioDeviceSettings next = settings_;
                           next.outputDevice = name;
                           choose(next);
                       };
                       items.push_back(std::move(item));
                   };
                   add("System default", "");
                   for (const auto& d : devices_)
                       if (d.hostApi == hostApiName() && d.outputs > 0)
                           add(d.name + " (" + std::to_string(d.outputs) + " out)", d.name);
                   openMenu("audio.output", std::move(items));
               });
        const bool asio = isAsio(host);
        const std::string inputText = !settings_.record               ? "None (playback only)"
                                      : asio                          ? "Same device as output"
                                      : settings_.inputDevice.empty() ? "System default"
                                                                      : settings_.inputDevice;
        button("audio.input", "Input device", inputText, [this, asio] {
            std::vector<ui::MenuItem> items;
            auto add = [&](std::string label, bool record, const std::string& name) {
                ui::MenuItem item;
                item.label = std::move(label);
                item.checked = settings_.record == record &&
                               (!record || asio || settings_.inputDevice == name);
                item.run = [this, record, name] {
                    AudioDeviceSettings next = settings_;
                    next.record = record;
                    if (record) next.inputDevice = name;
                    choose(next);
                };
                items.push_back(std::move(item));
            };
            add("None (playback only)", false, "");
            if (asio) {
                add("Same device as output", true, "");
            } else {
                add("System default", true, "");
                for (const auto& d : devices_)
                    if (d.hostApi == hostApiName() && d.inputs > 0)
                        add(d.name + " (" + std::to_string(d.inputs) + " in)", true, d.name);
            }
            openMenu("audio.input", std::move(items));
        });
    }

    const std::string automatic = backend == "jack"        ? "The graph's own rate"
                                  : backend == "portaudio" ? "The device's own rate"
                                                           : "The project's rate";
    button("audio.rate", "Sample rate", settings_.rate > 0 ? hz(settings_.rate) : automatic,
           [this, automatic] {
               std::vector<ui::MenuItem> items;
               auto add = [&](std::string label, double rate) {
                   ui::MenuItem item;
                   item.label = std::move(label);
                   item.checked = settings_.rate == rate;
                   item.run = [this, rate] {
                       AudioDeviceSettings next = settings_;
                       next.rate = rate;
                       choose(next);
                   };
                   items.push_back(std::move(item));
               };
               add(automatic, 0);
               for (double rate : kRates) add(hz(rate), rate);
               openMenu("audio.rate", std::move(items));
           });
    const double shownRate = settings_.rate > 0 ? settings_.rate : 48000;
    button("audio.block", "Buffer size", frames(settings_.block, shownRate), [this, shownRate] {
        std::vector<ui::MenuItem> items;
        for (unsigned block : kBlocks) {
            ui::MenuItem item;
            item.label = frames(block, shownRate);
            item.checked = settings_.block == block;
            item.run = [this, block] {
                AudioDeviceSettings next = settings_;
                next.block = block;
                choose(next);
            };
            items.push_back(std::move(item));
        }
        openMenu("audio.block", std::move(items));
    });
    if (backend == "jack")
        button("audio.graph", "PipeWire graph",
               settings_.forceGraph ? "Force" : "Request (shared with apps)", [this] {
                   std::vector<ui::MenuItem> items;
                   for (const bool force : {false, true}) {
                       ui::MenuItem item;
                       item.label = force ? "Force" : "Request (shared with apps)";
                       item.checked = settings_.forceGraph == force;
                       item.run = [this, force] {
                           AudioDeviceSettings next = settings_;
                           next.forceGraph = force;
                           choose(next);
                       };
                       items.push_back(std::move(item));
                   }
                   openMenu("audio.graph", std::move(items));
               });

    // What the choices mean here.
    std::vector<std::string> notes;
    if (backend == "jack") {
        notes = {"Rate and buffer size are requests: PipeWire changes rate only while idle",
                 "and allowed. Force holds both on the whole graph."};
    } else if (backend == "dummy") {
        notes = {"No device: a timer paces the engine and nothing is heard or recorded."};
    } else if (isAsio(host)) {
        notes = {"ASIO uses one device for input and output; the driver may round the buffer."};
    } else if (host.find("WASAPI") != std::string::npos) {
        notes = {"Shared mode: Windows converts other rates; its period can exceed the buffer."};
    } else {
        notes = {"A chosen rate switches the device's rate; the buffer is its I/O size."};
    }
    for (std::size_t i = 0; i < notes.size(); ++i)
        note(*form, "audio.note." + std::to_string(i), notes[i]);
    for (int i = 0; i < 3; ++i) note(*form, "audio.status." + std::to_string(i), "");

    auto& actions = form->emplace<ui::Node>("audio.actions");
    actions.layout.dir = ui::Layout::Dir::Row;
    actions.layout.height = ui::SizeRule::fixed(28);
    actions.layout.gap = 8;
    actions.layout.align = ui::Layout::Align::Center;
    actions.emplace<ui::Node>("audio.actions.space").layout.width = ui::SizeRule::fraction();
    actions.emplace<ui::Button>("audio.restart", "Restart audio", [this] {
        if (auto st = restart(); !st && onError) onError("audio device: " + message(st.error()));
    });
    actions.emplace<ui::Button>("audio.stop", "Stop audio", [this] { stop(); });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    refresh();
}

void AudioDeviceDialog::refresh() {
    auto* first = dynamic_cast<ui::Label*>(root_.find("audio.status.0"));
    if (!first) return;
    std::string text;
    if (app_.backend()) {
        const engine::EngineStatus status = app_.engineStatus();
        const std::string name = status.backend == "jack"        ? "JACK"
                                 : status.backend == "portaudio" ? resolvedHost(running_, devices_)
                                                                 : status.backend;
        char line[160];
        std::snprintf(line, sizeof line,
                      "Running: %s at %.0f Hz, block %lld, device period %lld, output %.1f ms.",
                      name.c_str(), status.sampleRate, static_cast<long long>(status.block),
                      static_cast<long long>(status.period),
                      status.sampleRate > 0
                          ? 1000.0 * double(status.deviceLatency) / status.sampleRate
                          : 0.0);
        text = line;
        if (status.restartNeeded) text += " The device changed or went away: Restart audio.";
        if (!lastError_.empty()) text += " " + lastError_;
    } else {
        text = lastError_.empty() ? "Stopped." : "Not running: " + lastError_;
    }
    // Labels are single-line; split the text into readable lines.
    for (int i = 0; i < 3; ++i) {
        auto* label = dynamic_cast<ui::Label*>(root_.find("audio.status." + std::to_string(i)));
        if (!label) break;
        std::size_t end = text.size();
        if (end > 84) {
            end = text.rfind(' ', 84);
            if (end == std::string::npos || end == 0) end = 84;
        }
        const bool visible = !text.empty();
        if (label->visible != visible) {
            label->visible = visible;
            label->relayout();
        }
        label->setText(text.substr(0, end));
        text.erase(0, end);
        if (!text.empty() && text.front() == ' ') text.erase(0, 1);
    }
}

}  // namespace supple::gui
