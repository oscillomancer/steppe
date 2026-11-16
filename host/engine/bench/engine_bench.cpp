// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// B1 capacity: command-built stock chains, a monitored input and master limiter.
#include <sys/resource.h>
#include "measurements.h"
#include "session.h"
#include "supple/product.h"

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "plan.h"
#include "supple/base/environment.h"
#include "supple/base/files.h"
#include "supple/base/thread.h"
#include "supple/engine/jack.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"

using namespace supple;
using namespace supple::engine;
namespace {
// What the kernel says about each thread of the bench and of its bridge
// children (Linux): its class, the switches it made and suffered, its
// migrations, how long it waited runnable for a CPU (schedstat run_delay) and
// the page faults it took. Read before and after the measured interval, off
// the audio path.
struct TaskSched {
    std::string process, comm;
    long long policy = 0, prio = 0, voluntary = 0, involuntary = 0, migrations = 0;
    long long minorFaults = 0, majorFaults = 0;
    long long cpuNs = 0, runDelayNs = 0, slices = 0;
    // With kernel.sched_schedstats=1 (else -1), in ms since the reset at the
    // interval's start: the longest wait runnable for a CPU, and the longest
    // run without a switch; the waits' sum and count.
    double waitMaxMs = -1, execMaxMs = -1, waitSumMs = -1;
    long long waitCount = -1;
};
using SchedSnapshot = std::map<std::string, TaskSched>;

// `reset`: clear each thread's scheduler statistics first (allowed for one's
// own threads), so the maxima cover what follows.
SchedSnapshot schedSnapshot(bool reset = false) {
    SchedSnapshot out;
#if defined(__linux__)
    namespace fs = std::filesystem;
    const std::string self = std::to_string(::getpid());
    std::vector<std::string> pids{self};
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        const std::string pid = entry.path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::ifstream stat(entry.path() / "stat");
        std::string line;
        if (!std::getline(stat, line)) continue;
        const auto close = line.rfind(')');
        if (close == std::string::npos) continue;
        char state = 0;
        long long ppid = 0;
        if (std::sscanf(line.c_str() + close + 1, " %c %lld", &state, &ppid) == 2 &&
            std::to_string(ppid) == self)
            pids.push_back(pid);
    }
    for (const auto& pid : pids) {
        std::ifstream pcomm("/proc/" + pid + "/comm");
        std::string process;
        std::getline(pcomm, process);
        for (const auto& task : fs::directory_iterator("/proc/" + pid + "/task", ec)) {
            TaskSched t;
            t.process = process;
            std::ifstream comm(task.path() / "comm");
            std::getline(comm, t.comm);
            if (reset) std::ofstream(task.path() / "sched") << "0";
            {
                // stat: after "(comm)", fields 3.. ; minflt is 10, majflt 12.
                std::ifstream tstat(task.path() / "stat");
                std::string tline;
                if (std::getline(tstat, tline)) {
                    const auto close = tline.rfind(')');
                    long long minflt = 0, majflt = 0;
                    if (close != std::string::npos &&
                        std::sscanf(tline.c_str() + close + 1,
                                    " %*c %*d %*d %*d %*d %*d %*u %lld %*u %lld", &minflt,
                                    &majflt) == 2) {
                        t.minorFaults = minflt;
                        t.majorFaults = majflt;
                    }
                }
            }
            std::ifstream schedstat(task.path() / "schedstat");
            schedstat >> t.cpuNs >> t.runDelayNs >> t.slices;
            std::ifstream sched(task.path() / "sched");
            std::string line;
            while (std::getline(sched, line)) {
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                key.erase(key.find_last_not_of(' ') + 1);
                const long long value = std::atoll(line.c_str() + colon + 1);
                if (key == "nr_voluntary_switches")
                    t.voluntary = value;
                else if (key == "nr_involuntary_switches")
                    t.involuntary = value;
                else if (key == "se.nr_migrations")
                    t.migrations = value;
                else if (key == "policy")
                    t.policy = value;
                else if (key == "prio")
                    t.prio = value;
                else if (key == "wait_max")
                    t.waitMaxMs = std::atof(line.c_str() + colon + 1);
                else if (key == "exec_max")
                    t.execMaxMs = std::atof(line.c_str() + colon + 1);
                else if (key == "wait_sum")
                    t.waitSumMs = std::atof(line.c_str() + colon + 1);
                else if (key == "wait_count")
                    t.waitCount = value;
            }
            out[pid + "/" + task.path().filename().string()] = std::move(t);
        }
    }
#endif
    return out;
}

// The interval's real-time threads (and the busiest others) with their deltas.
json::Value schedDelta(const SchedSnapshot& before, const SchedSnapshot& after) {
    auto threads = json::Value::array();
    for (const auto& [id, t] : after) {
        const auto was = before.find(id);
        const TaskSched z;
        const TaskSched& b = was == before.end() ? z : was->second;
        const long long delay = t.runDelayNs - b.runDelayNs;
        const long long minor = t.minorFaults - b.minorFaults;
        const long long major = t.majorFaults - b.majorFaults;
        if (t.policy == 0 && delay < 1'000'000 && minor == 0 && major == 0) continue;
        auto e = json::Value::object();
        e["id"] = id;
        e["process"] = t.process;
        e["comm"] = t.comm;
        e["policy"] = t.policy;
        e["prio"] = t.prio;
        e["cpu_ms"] = static_cast<double>(t.cpuNs - b.cpuNs) / 1e6;
        e["run_delay_ms"] = static_cast<double>(delay) / 1e6;
        e["slices"] = t.slices - b.slices;
        e["voluntary"] = t.voluntary - b.voluntary;
        e["involuntary"] = t.involuntary - b.involuntary;
        e["migrations"] = t.migrations - b.migrations;
        e["minor_faults"] = minor;
        e["major_faults"] = major;
        if (t.waitMaxMs >= 0) {
            e["wait_max_ms"] = t.waitMaxMs;
            e["wait_sum_ms"] = t.waitSumMs;
            e["wait_count"] = t.waitCount;
            e["exec_max_ms"] = t.execMaxMs;
        }
        threads.push_back(std::move(e));
    }
    return threads;
}

// Supplies silent monitored inputs and silences the physical outputs after DSP.
// JACK owns whole-period timing; the paced thread owns synthetic timing. With
// a callback priority the paced thread and live helpers run SCHED_FIFO at it,
// as JACK's would.
class BenchBackend final : public Backend {
  public:
    BenchBackend(bool real, BenchProbe* probe, int callbackPriority)
        : probe_(probe), callbackPriority_(callbackPriority) {
        if (real)
            jack_ = std::make_unique<JackBackend>(
                JackOptions{.clientName = "supple-capacity-bench", .benchProbe = probe});
    }
    Status start(const BackendConfig& config, Callback cb) override {
        config_ = config;
        callback_ = std::move(cb);
        input_.assign(config.blockSize, 0.f);
        left_.assign(config.blockSize, 0.f);
        right_.assign(config.blockSize, 0.f);
        if (jack_) {
            auto physical = config;
            physical.inputs = 0;  // no capture device opened
            auto result = jack_->start(physical, [this](const float* const*, float* const* out,
                                                        std::uint32_t n) { call(out, n); });
            config_.sampleRate = jack_->config().sampleRate;
            return result;
        }
        running_ = true;
        thread_ = std::thread([this] {
            nameThread("audio-paced");
            const auto period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(config_.blockSize / config_.sampleRate));
            auto next = Clock::now();
            float* out[2] = {left_.data(), right_.data()};
            while (running_.load()) {
                const bool take = recording.load(std::memory_order_relaxed);
                const auto start = Clock::now();
                call(out, config_.blockSize);
                const auto elapsed = Clock::now() - start;
                if (take) {
                    if (elapsed > period) ++overBudget;
                    if (probe_)
                        probe_->callback.append(
                            std::chrono::duration<double, std::micro>(elapsed).count(),
                            config_.blockSize);
                }
                ++cycles_;
                next += period;
                const auto now = Clock::now();
                if (now > next + period) {
                    if (take) ++lateWakes;
                    next = now;  // no catch-up busy loop
                }
                std::this_thread::sleep_until(next);
            }
            call(out, 0);
        });
        if (callbackPriority_ > 0)
            if (auto st = setThreadPolicy(thread_.native_handle(), fifo()); !st) {
                stop();
                return st;
            }
        return {};
    }
    void call(float* const* out, std::uint32_t n) SUPPLE_RT {
        const float* in[2] = {input_.data(), input_.data()};
        callback_(in, out, n);
        if (recording.load(std::memory_order_relaxed))
            for (std::uint32_t c = 0; c < 2; ++c)
                for (std::uint32_t i = 0; i < n; ++i)
                    peak = std::max(peak, std::abs(static_cast<double>(out[c][i])));
        if (jack_)
            for (std::uint32_t c = 0; c < 2; ++c) std::fill_n(out[c], n, 0.f);
    }
    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (jack_) jack_->stop();
    }
    BackendConfig config() const override { return config_; }
    std::string name() const override { return jack_ ? "jack" : "paced"; }
    bool ownsThread() const override { return true; }
    unsigned liveThreadCapacity() const override {
        return jack_ ? jack_->liveThreadCapacity() : Backend::liveThreadCapacity();
    }
    Status prepareLiveThread(std::thread::native_handle_type thread) override {
        if (jack_) return jack_->prepareLiveThread(thread);
        return callbackPriority_ > 0 ? setThreadPolicy(thread, fifo()) : okStatus();
    }
    ThreadPolicy callbackPolicy() override {
        if (jack_) return jack_->callbackPolicy();
        return thread_.joinable() ? threadPolicy(thread_.native_handle()) : ThreadPolicy{};
    }
    BackendStatus status() const override {
        return jack_ ? jack_->status() : BackendStatus{.period = config_.blockSize};
    }
    std::uint64_t cycles() const { return jack_ ? jack_->stats().cycles : cycles_.load(); }
    ~BenchBackend() override { stop(); }
    std::atomic<bool> recording{false};
    std::uint64_t overBudget = 0, lateWakes = 0;  // read after stop only
    double peak = 0;

  private:
    using Clock = std::chrono::steady_clock;
    ThreadPolicy fifo() const { return {ThreadPolicy::Kind::Fifo, callbackPriority_}; }
    BenchProbe* probe_;
    int callbackPriority_;
    std::unique_ptr<JackBackend> jack_;
    BackendConfig config_;
    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> cycles_{0};
    std::vector<float> input_, left_, right_;
};

int fail(const Error& error) {
    std::fprintf(stderr, "%s\n", error.toString().c_str());
    return 1;
}
int stripBench() {
    using namespace supple::engine::detail;
    std::printf("Console 9 engine strips, stereo, 48 kHz, block 512, 2097152 frames/row\n");
    std::printf("| strip | mode | ns/frame | one 48 kHz strip, %% core |\n|---|---|---:|---:|\n");
    for (bool on : {false, true})
        for (bool buss : {false, true}) {
            model::EditTxn edit(model::Session::create({.console9 = on, .seed = 6789}), 1);
            model::Track t;
            t.parent = model::kMaster;
            if (buss) t.type = model::StripType::Buss;
            const Id target = edit.create(t);
            edit.modify<model::Track>(model::kMaster,
                                      [&](auto& m) { m.children.push_back(target); });
            if (buss) {
                model::Track ch;
                ch.parent = target;
                const Id source = edit.create(ch);
                edit.modify<model::Track>(target, [&](auto& b) { b.children.push_back(source); });
            }
            auto session = edit.commit();
            if (!session) return 1;
            InstancePool pool([](const model::PluginRef&) -> Result<host::PluginInfo> {
                return Error("bench.plugin", "strip benchmark uses no plugins");
            });
            auto compiled = compile(*session, pool, 0, 512);
            if (!compiled || !prepare(*compiled, *session)) return 1;
            auto& p = *compiled;
            const auto at = static_cast<std::size_t>(
                std::find_if(p.nodes.begin(), p.nodes.end(),
                             [&](const auto& n) { return n.id == target; }) -
                p.nodes.begin());
            // A channel's device input, or an already encoded upstream signal for a buss.
            std::vector<float> input(512, .01f);
            const float* in[] = {input.data()};
            p.deviceIn = in;
            p.deviceInChannels = 1;
            if (buss) {
                for (auto& n : p.nodes)
                    if (n.type == model::StripType::Channel) {
                        std::fill(n.post.l.begin(), n.post.l.end(), .0054f);
                        std::fill(n.post.r.begin(), n.post.r.end(), .0054f);
                    }
            } else
                p.nodes[at].monitorInputs = {0};
            const auto transport = transportAt(p, 0);
            for (int i = 0; i < 32; ++i) processNode(p, at, 0, 512, transport);
            const auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < 4096; ++i) processNode(p, at, i * 512, 512, transport);
            const double ns =
                std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin)
                    .count() /
                2097152;
            std::printf("| %s | %s | %.2f | %.3f |\n", buss ? "buss" : "channel",
                        on ? "on" : "linear", ns, ns * 48000 / 1e7);
        }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--strips") return stripBench();
    // One search/report implementation serves both hosts; exec replaces this
    // process, preserving exit/signal semantics and forwarding the exact argv.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--search") continue;
        std::vector<std::string> args = {"python3", BENCH_DRIVER, "b1", "--engine", argv[0]};
        for (int j = 1; j < argc; ++j) args.emplace_back(argv[j]);
        std::vector<char*> pointers;
        for (auto& arg : args) pointers.push_back(arg.data());
        pointers.push_back(nullptr);
        execvp(pointers[0], pointers.data());
        std::perror("could not start benchmark search driver");
        return 1;
    }
    std::string device = "paced", pluginDir = BENCH_PLUGINS, recipePath = BENCH_RECIPE;
    std::string output, exportPath, pluginFile, pluginId, input;
    std::string background = "auto", isolation = "in_process";
    int block = 64, workers = -1, fxPerTrack = 1, capacity = 500000;
    int backgroundPriority = 0, callbackPriority = 0, parkUs = 0;
    bool live = false, metrics = true;
    int tracks = 2;
    double seconds = 300;
    bool anticipative = true;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::puts(
                    "engine-bench [--device paced|jack] [--tracks N] [--seconds 300] "
                    "[--anticipative on|off] [--plugins DIR] [--output JSON] "
                    "[--block 64|128] [--workers -1] [--metrics on|off] "
                    "[--sample-capacity 500000] [--plugin-file PATH --plugin-id ID --input WAV "
                    "--live on|off --fx-per-track N] [--background auto|normal|fifo] "
                    "[--background-priority 0] [--callback-priority 0] [--park-us 0] "
                    "[--isolation in_process|shared_bridge|dedicated] "
                    "[--export-session JSON] [--search --max-tracks 256 --report "
                    "docs/benchmarks/b1.md]");
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[i];
            if (arg == "--device")
                device = value;
            else if (arg == "--plugins")
                pluginDir = value;
            else if (arg == "--plugin-file")
                pluginFile = value;
            else if (arg == "--plugin-id")
                pluginId = value;
            else if (arg == "--input")
                input = value;
            else if (arg == "--background" &&
                     (value == "auto" || value == "normal" || value == "fifo"))
                background = value;
            else if (arg == "--isolation" &&
                     (value == "in_process" || value == "shared_bridge" || value == "dedicated"))
                isolation = value;
            else if (arg == "--block" || arg == "--workers" || arg == "--fx-per-track" ||
                     arg == "--sample-capacity" || arg == "--background-priority" ||
                     arg == "--callback-priority" || arg == "--park-us") {
                std::size_t used;
                const int number = std::stoi(value, &used);
                if (used != value.size()) throw std::invalid_argument("invalid integer");
                if (arg == "--block")
                    block = number;
                else if (arg == "--workers")
                    workers = number;
                else if (arg == "--fx-per-track")
                    fxPerTrack = number;
                else if (arg == "--background-priority")
                    backgroundPriority = number;
                else if (arg == "--callback-priority")
                    callbackPriority = number;
                else if (arg == "--park-us")
                    parkUs = number;
                else
                    capacity = number;
            } else if ((arg == "--live" || arg == "--metrics") &&
                       (value == "on" || value == "off")) {
                if (arg == "--live")
                    live = value == "on";
                else
                    metrics = value == "on";
            } else if (arg == "--recipe")
                recipePath = value;
            else if (arg == "--output")
                output = value;
            else if (arg == "--export-session")
                exportPath = value;
            else if (arg == "--tracks") {
                std::size_t used;
                tracks = std::stoi(value, &used);
                if (used != value.size()) throw std::invalid_argument("invalid track count");
            } else if (arg == "--seconds") {
                std::size_t used;
                seconds = std::stod(value, &used);
                if (used != value.size()) throw std::invalid_argument("invalid duration");
            } else if (arg == "--anticipative" && (value == "on" || value == "off"))
                anticipative = value == "on";
            else
                throw std::invalid_argument("unknown option/value: " + arg);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s; use --help\n", e.what());
        return 2;
    }
    if ((device != "paced" && device != "jack") || tracks < 1 || tracks > 4096 ||
        !std::isfinite(seconds) || seconds <= 0 || (block != 64 && block != 128) || workers < -1 ||
        workers > 64 || fxPerTrack < 1 || fxPerTrack > 64 || capacity < 1 || capacity > 4000000 ||
        (pluginFile.empty() && (!pluginId.empty() || !input.empty() || live || fxPerTrack != 1)) ||
        (!pluginFile.empty() && (pluginId.empty() || input.empty() || !exportPath.empty())) ||
        backgroundPriority < 0 || backgroundPriority > 99 || callbackPriority < 0 ||
        callbackPriority > 99 || (device == "jack" && callbackPriority != 0)) {
        std::fprintf(stderr, "invalid device, track count or duration\n");
        return 2;
    }
    if (device == "jack" && !environmentFlagEnabled("SUPPLE_AUDIO_DEVICE_TEST"))
        return fail(Error("bench.opt_in", "JACK requires SUPPLE_AUDIO_DEVICE_TEST=1"));
    auto text = readTextFile(recipePath);
    if (!text) return fail(text.error());
    auto recipe = json::parse(*text);
    if (!recipe) return fail(recipe.error());
    (void)host::locateBridgeExecutable(std::string(product::kSlug) + "-bridge");
    auto app = EngineApp::create({.pluginPath = {pluginFile.empty() ? pluginDir : pluginFile},
                                  .workdir = std::filesystem::current_path()});
    if (!app) return fail(app.error());
    auto built = pluginFile.empty()
                     ? bench::buildSession(**app, *recipe, tracks)
                     : bench::buildEffectSession(**app, pluginId, input, tracks, live, fxPerTrack);
    if (!built) return fail(built.error());
    if (isolation != "in_process")
        if (auto isolated = bench::isolateAll(**app, isolation); !isolated)
            return fail(isolated.error());
    if (!exportPath.empty()) {
        auto exported = bench::exportSession(**app);
        if (!exported) return fail(exported.error());
        auto st = writeFileAtomic(exportPath, json::writeCanonical(*exported) + "\n");
        return st ? 0 : fail(st.error());
    }
    // Even inherited forcing variables are rejected: this benchmark requests
    // client latency only and records whatever period the graph grants.
    if (device == "jack" && (std::getenv("PIPEWIRE_QUANTUM") || std::getenv("PIPEWIRE_RATE")))
        return fail(Error("bench.forced_graph", "forced PipeWire quantum/rate in environment", "",
                          "unset PIPEWIRE_QUANTUM and PIPEWIRE_RATE before benchmarking"));
    BenchProbe probe(metrics ? static_cast<std::size_t>(capacity) : 0);
    BenchBackend backend(device == "jack", metrics ? &probe : nullptr, callbackPriority);
    using Background = EngineOptions::Background;
    auto started = (*app)->startEngine(
        backend, {.sampleRate = 48000, .blockSize = static_cast<std::uint32_t>(block), .inputs = 2},
        {.anticipative = anticipative,
         .workers = workers,
         .benchProbe = metrics ? &probe : nullptr,
         .liveHelperParkUs = static_cast<double>(parkUs),
         .background = background == "normal" ? Background::Normal
                       : background == "fifo" ? Background::Fifo
                                              : Background::Auto,
         .backgroundPriority = backgroundPriority});
    if (!started) {
        (*app)->stopEngine();
        return fail(started.error());
    }
    auto* engine = (*app)->engine();
    if (!engine->awaitPlan(30)) {
        (*app)->stopEngine();
        return fail(Error("bench.prepare", "plan adoption timed out"));
    }
    engine->play();
    engine->awaitTransport();
    auto service = [&](double duration) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration);
        while (std::chrono::steady_clock::now() < end) {
            if (!bench::serviceTick(**app, engine)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    };
    auto changedDevice = [&] {
        (*app)->stopEngine();  // cancel any pending automatic restart
        return fail(Error("bench.device_changed", "device restart interrupted the measurement", "",
                          "retry with a stable device; this run has no capacity result"));
    };
    // Exclude activation/preroll; keep plug-in main-thread service running.
    if (!service(1)) return changedDevice();
    const auto before = backend.status();
    const auto m0 = engine->anticipationMisses(), e0 = engine->processErrors();
    host::resetBridgeTiming();  // the measured interval's bridged calls only
    const auto o0 = engine->bridgeOverruns();
    const auto c0 = backend.cycles();
    const SchedSnapshot s0 = schedSnapshot(true);
    const auto f0 = (*app)->engineStatus().audioPageFaults;
    rusage ru0{}, ru1{};
    getrusage(RUSAGE_SELF, &ru0);
    const auto begin = std::chrono::steady_clock::now();
    probe.recording = true;
    backend.recording = true;
    const bool serviced = service(seconds);
    backend.recording = false;
    probe.recording = false;
    if (!serviced) return changedDevice();
    const double measured =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    getrusage(RUSAGE_SELF, &ru1);
    const SchedSnapshot s1 = schedSnapshot();
    const auto f1 = (*app)->engineStatus().audioPageFaults;
    const auto thp = (*app)->engineStatus().thpOptOut;
    const auto after = backend.status();
    const double rate = backend.config().sampleRate;
    auto result = json::Value::object();
    result["schema"] = "supple.capacity-run/2.4";
    result["tracks"] = tracks;
    result["workload"] = pluginFile.empty() ? "stock" : "effect";
    result["plugin_file"] = pluginFile;
    result["plugin_id"] = pluginId;
    result["input"] = input;
    result["live"] = live;
    result["fx_per_track"] = pluginFile.empty() ? 3 : fxPerTrack;
    result["fx_count"] = (*app)->head().fx.size();
    result["workers"] = workers;
    result["live_helpers"] = engine->liveHelpers();
    result["isolation"] = isolation;
    result["background"] = background;
    result["background_priority"] = backgroundPriority;
    result["callback_priority"] = callbackPriority;
    result["park_us"] = parkUs;
    auto scheduling = json::Value::object();
    const Engine::Scheduling& applied = engine->scheduling();
    for (const auto& [name, policy] :
         {std::pair{"callback", applied.callback}, std::pair{"live", applied.live},
          std::pair{"renderer", applied.renderer}, std::pair{"background", applied.background}}) {
        auto entry = json::Value::object();
        entry["policy"] = std::string(policyName(policy.kind));
        entry["priority"] = policy.priority;
        scheduling[name] = std::move(entry);
    }
    result["scheduling"] = std::move(scheduling);
    result["device"] = device;
    result["anticipative"] = anticipative;
    result["seconds"] = measured;
    result["sample_rate"] = rate;
    result["requested_block"] = block;
    result["quantum"] = after.period;
    result["requested_settings_granted"] =
        rate == 48000 && after.period == static_cast<std::uint32_t>(block);
    result["xruns"] = device == "jack" ? json::Value(after.xruns - before.xruns) : json::Value();
    result["anticipation_misses"] = engine->anticipationMisses() - m0;
    result["process_errors"] = engine->processErrors() - e0;
    {
        // Where live bridged calls spent the interval, and why cycles ran late.
        auto o1 = engine->bridgeOverruns();
        o1.lateWait -= o0.lateWait;
        o1.slowBridge -= o0.slowBridge;
        o1.notWaiting -= o0.notWaiting;
        const auto t = bridgeTimingStatus(host::bridgeTiming().live, o1);
        json::Value b = json::Value::object();
        b["calls"] = t.calls;
        b["late"] = t.late;
        b["past_deadline"] = t.pastDeadline;
        b["spun"] = t.spun;
        b["wait_max_us"] = t.waitMaxUs;
        b["wait_mean_us"] = t.waitMeanUs;
        b["wake_max_us"] = t.wakeMaxUs;
        b["wake_mean_us"] = t.wakeMeanUs;
        b["work_max_us"] = t.workMaxUs;
        b["return_max_us"] = t.returnMaxUs;
        b["wakes_over_200us"] = t.wakesOver200us;
        b["wakes_over_1ms"] = t.wakesOver1ms;
        // Under 10, 20, 50, 100, 200, 500, 1000, 2000 µs, and at or over 2 ms.
        const host::BridgeTiming::Calls liveCalls = host::bridgeTiming().live;
        b["wake_buckets"] = json::Value::array();
        b["return_buckets"] = json::Value::array();
        for (std::size_t k = 0; k < liveCalls.wakeBuckets.size(); ++k) {
            b["wake_buckets"].push_back(static_cast<std::int64_t>(liveCalls.wakeBuckets[k]));
            b["return_buckets"].push_back(static_cast<std::int64_t>(liveCalls.returnBuckets[k]));
        }
        b["bridge_cpus"] = json::Value::array();
        for (auto c : t.bridgeCpus) b["bridge_cpus"].push_back(c);
        b["migrations"] = t.migrations;
        b["class_mismatches"] = t.classMismatches;
        b["longest_cycle_us"] = t.longestCycleUs;  // since the engine started
        b["overruns_late_wait"] = t.overrunsLateWait;
        b["overruns_slow_bridge"] = t.overrunsSlowBridge;
        b["overruns_not_waiting"] = t.overrunsNotWaiting;
        result["bridge_timing"] = std::move(b);
    }
    result["threads"] = schedDelta(s0, s1);
    result["cycles"] = backend.cycles() - c0;
    result["restart_needed"] = after.restartNeeded;
    result["warnings"] = json::Value::array();
    for (const auto& warning : engine->warnings()) result["warnings"].push_back(warning);
    const bool healthy = result["cycles"].asInt() > 0 && !after.restartNeeded &&
                         engine->processErrors() == 0 && engine->warnings().empty();
    (*app)->stopEngine();  // joins both producers before inspecting their storage
    result["metrics_enabled"] = metrics;
    result["callback"] = metrics ? bench::summarize(probe.callback, rate) : json::Value();
    result["render_job"] = metrics ? bench::summarize(probe.renderJob, rate) : json::Value();
    result["ring_low_water_frames"] =
        probe.aheadObservations ? json::Value(probe.aheadLowWater) : json::Value();
    result["ring_observations"] = probe.aheadObservations;
    result["synthetic_over_budget"] =
        device == "paced" ? json::Value(backend.overBudget) : json::Value();
    result["synthetic_late_wakes"] =
        device == "paced" ? json::Value(backend.lateWakes) : json::Value();
    result["output_peak"] = backend.peak;
    auto cpu = [](const rusage& r) {
        return r.ru_utime.tv_sec + r.ru_utime.tv_usec * 1e-6 + r.ru_stime.tv_sec +
               r.ru_stime.tv_usec * 1e-6;
    };
    result["process_cpu_cores"] = (cpu(ru1) - cpu(ru0)) / measured;
    result["minor_faults"] = ru1.ru_minflt - ru0.ru_minflt;
    result["major_faults"] = ru1.ru_majflt - ru0.ru_majflt;
    {
        // As engine.status counts them: the callback's and live helpers' (Linux).
        auto faults = json::Value::object();
        faults["scope"] = f1.scope;
        faults["minor"] = f1.minor - f0.minor;
        faults["major"] = f1.major && f0.major ? json::Value(*f1.major - *f0.major) : json::Value();
        faults["threads"] = f1.threads;
        result["audio_page_faults"] = std::move(faults);
        result["thp_opt_out"] = thp.active ? (thp.advisedOnly ? "advised_only" : "all") : "off";
    }
    // Silence means a missing signal unless the anticipative renderer fell
    // behind: then its starved ring explains it and the run is a failure.
    const bool signal = backend.peak > 1e-5 || result["anticipation_misses"].asInt() > 0;
    const bool valid = healthy && signal && !probe.callback.dropped && !probe.renderJob.dropped;
    result["valid"] = valid;
    const auto serialized = json::writeCanonical(result) + "\n";
    if (!output.empty()) {
        auto st = writeFileAtomic(output, serialized);
        if (!st) return fail(st.error());
    }
    std::printf("%s", serialized.c_str());
    return valid ? 0 : 1;
}
