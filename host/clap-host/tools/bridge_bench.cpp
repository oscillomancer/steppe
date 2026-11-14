// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
//
// B6 (engine.md §7, ADR-0013): what a bridged process() call costs against
// an in-process one. Runs the hermetic gain fixture in each isolation mode,
// for each block size and instance count, as a callback would: one cycle per
// period (paced by the steady clock), every instance processed in turn on the
// calling thread. Reports per-call and per-cycle times, and the cycles whose
// bridged calls took longer than their period. Measurement only: it decides
// nothing, and its numbers depend on the machine (record kernel, governor,
// CPU and scheduling with them).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "supple/base/thread.h"
#include "supple/host/clap_host.h"
#include "supple/json/json.h"

using namespace supple;
using namespace supple::host;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::filesystem::path fixture = FIXTURE_CLAP_PATH;
    std::filesystem::path bridge = BRIDGE_PATH;
    std::vector<int> blocks{64, 128, 256, 1024};
    std::vector<int> instances{1, 8, 32};
    std::vector<std::string> modes{"in_process", "shared_bridge", "dedicated"};
    int cycles = 2000, warmup = 200;
    int callerPriority = 0, bridgePriority = 0;  // 0: normal scheduling
    bool paced = true;
    std::string output;
};

std::vector<int> numbers(const std::string& text) {
    std::vector<int> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t comma = std::min(text.find(',', at), text.size());
        std::size_t used = 0;
        const std::string part = text.substr(at, comma - at);
        out.push_back(std::stoi(part, &used));
        if (used != part.size()) throw std::invalid_argument("invalid number: " + part);
        at = comma + 1;
    }
    return out;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

json::Value summary(const std::vector<double>& us) {
    json::Value s = json::Value::object();
    s["p50_us"] = percentile(us, 0.5);
    s["p99_us"] = percentile(us, 0.99);
    s["p999_us"] = percentile(us, 0.999);
    s["max_us"] = us.empty() ? 0.0 : *std::max_element(us.begin(), us.end());
    return s;
}

Result<json::Value> run(const Options& o, const PluginInfo& gain, const std::string& mode,
                        int block, int count) {
    std::shared_ptr<BridgeProcess> shared;
    if (mode == "shared_bridge") {
        auto made = createBridgeProcess();
        if (!made) return std::move(made).error();
        shared = std::move(*made);
    }
    std::vector<std::unique_ptr<Instance>> chain;
    for (int i = 0; i < count; ++i) {
        auto made = Instance::create(gain, {}, mode != "in_process", shared);
        if (!made) return std::move(made).error();
        SUPPLE_TRY((*made)->activate(48000, 1, static_cast<std::uint32_t>(block)));
        if (!(*made)->startProcessing())
            return Error("bench.start", mode + ": start_processing failed");
        chain.push_back(std::move(*made));
    }
    const auto frames = static_cast<std::uint32_t>(block);
    std::vector<float> l(frames, 0.25f), r(frames, -0.25f), ol(frames), orr(frames);
    float* in[2] = {l.data(), r.data()};
    float* out[2] = {ol.data(), orr.data()};
    ProcessBlock b;
    b.inputs = in;
    b.inputChannels = 2;
    b.outputs = out;
    b.outputChannels = 2;
    b.frames = frames;
    const auto period =
        std::chrono::nanoseconds(static_cast<long long>(block) * 1'000'000'000 / 48000);
    const double periodUs = static_cast<double>(period.count()) / 1000.0;
    std::vector<double> calls, cycles;
    calls.reserve(static_cast<std::size_t>(o.cycles) * chain.size());
    cycles.reserve(static_cast<std::size_t>(o.cycles));
    int over = 0;
    auto next = Clock::now();
    for (int c = 0; c < o.warmup + o.cycles; ++c) {
        if (o.paced) {
            next += period;
            std::this_thread::sleep_until(next);
        }
        const auto start = Clock::now();
        for (auto& instance : chain) {
            const auto t0 = Clock::now();
            if (!instance->process(b)) return Error("bench.process", mode + ": process failed");
            const auto t1 = Clock::now();
            if (c >= o.warmup)
                calls.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        if (c < o.warmup) continue;
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        cycles.push_back(us);
        if (us > periodUs) ++over;
    }
    for (auto& instance : chain) {
        if (instance->failed()) return Error("bench.failed", mode + ": " + instance->failure());
        instance->stopProcessing();
        instance->deactivate();
    }
    json::Value row = json::Value::object();
    row["isolation"] = mode;
    row["block"] = block;
    row["instances"] = count;
    row["period_us"] = periodUs;
    row["call"] = summary(calls);
    row["cycle"] = summary(cycles);
    row["cycles"] = o.cycles;
    row["cycles_over_period"] = over;
    return row;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::puts(
                    "bridge-bench [--blocks 64,128,256,1024] [--instances 1,8,32] "
                    "[--isolation all|in_process|shared_bridge|dedicated] [--cycles 2000] "
                    "[--warmup 200] [--paced on|off] [--caller-priority 0] "
                    "[--bridge-priority 0] [--output JSON]");
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("missing value for " + arg);
            const std::string value = argv[i];
            if (arg == "--blocks")
                o.blocks = numbers(value);
            else if (arg == "--instances")
                o.instances = numbers(value);
            else if (arg == "--isolation" && value != "all")
                o.modes = {value};
            else if (arg == "--isolation")
                continue;
            else if (arg == "--cycles")
                o.cycles = numbers(value).at(0);
            else if (arg == "--warmup")
                o.warmup = numbers(value).at(0);
            else if (arg == "--caller-priority")
                o.callerPriority = numbers(value).at(0);
            else if (arg == "--bridge-priority")
                o.bridgePriority = numbers(value).at(0);
            else if (arg == "--paced" && (value == "on" || value == "off"))
                o.paced = value == "on";
            else if (arg == "--output")
                o.output = value;
            else
                throw std::invalid_argument("unknown option " + arg);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s; use --help\n", e.what());
        return 2;
    }
    for (const auto& m : o.modes)
        if (m != "in_process" && m != "shared_bridge" && m != "dedicated") {
            std::fprintf(stderr, "unknown isolation %s\n", m.c_str());
            return 2;
        }
    if (o.cycles < 1 || o.warmup < 0 || o.callerPriority < 0 || o.callerPriority > 99 ||
        o.bridgePriority < 0 || o.bridgePriority > 99) {
        std::fprintf(stderr, "invalid cycles or priority\n");
        return 2;
    }
    for (int b : o.blocks)
        if (b < 16 || b > 4096) {
            std::fprintf(stderr, "block %d out of 16..4096\n", b);
            return 2;
        }
    for (int n : o.instances)
        if (n < 1 || n > 256) {
            std::fprintf(stderr, "instance count %d out of 1..256\n", n);
            return 2;
        }
    setBridgeExecutable(o.bridge);
    if (o.bridgePriority) setBridgeAudioPolicy({ThreadPolicy::Kind::Fifo, o.bridgePriority});
    ThreadPolicy caller{ThreadPolicy::Kind::Normal, 0};
    if (o.callerPriority) {
        caller = {ThreadPolicy::Kind::Fifo, o.callerPriority};
#if defined(_WIN32)
        const auto self = GetCurrentThread();
#else
        const auto self = pthread_self();
#endif
        if (auto set = setThreadPolicy(self, caller); !set) {
            std::fprintf(stderr, "%s\n", set.error().toString().c_str());
            return 1;
        }
    }
    auto found = scan(std::span<const std::filesystem::path>(&o.fixture, 1));
    if (!found) {
        std::fprintf(stderr, "%s\n", found.error().toString().c_str());
        return 1;
    }
    const auto gain = std::find_if(found->begin(), found->end(), [](const PluginInfo& p) {
        return p.id == "com.suppleaudio.test.gain";
    });
    if (gain == found->end()) {
        std::fprintf(stderr, "the gain fixture is missing from %s\n", o.fixture.c_str());
        return 1;
    }
    json::Value report = json::Value::object();
    report["schema"] = "supple.bridge-bench/1.0";
    report["paced"] = o.paced;
    report["caller"] = std::string(policyName(caller.kind)) + " " + std::to_string(caller.priority);
    report["bridge"] = o.bridgePriority ? "fifo " + std::to_string(o.bridgePriority) : "normal 0";
    json::Value rows = json::Value::array();
    std::printf("%-14s %6s %5s %9s %9s %9s %9s %9s %9s %6s\n", "isolation", "block", "n", "period",
                "call p50", "call p99", "cyc p50", "cyc p99", "cyc max", "over");
    for (const auto& mode : o.modes)
        for (int block : o.blocks)
            for (int count : o.instances) {
                auto row = run(o, *gain, mode, block, count);
                if (!row) {
                    std::fprintf(stderr, "%s\n", row.error().toString().c_str());
                    return 1;
                }
                json::Value& r = *row;
                std::printf("%-14s %6d %5d %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %6lld\n",
                            mode.c_str(), block, count, r["period_us"].asDouble(),
                            r["call"]["p50_us"].asDouble(), r["call"]["p99_us"].asDouble(),
                            r["cycle"]["p50_us"].asDouble(), r["cycle"]["p99_us"].asDouble(),
                            r["cycle"]["max_us"].asDouble(),
                            static_cast<long long>(r["cycles_over_period"].asInt()));
                std::fflush(stdout);
                rows.push_back(std::move(*row));
            }
    report["runs"] = std::move(rows);
    if (!o.output.empty()) {
        std::FILE* f = std::fopen(o.output.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", o.output.c_str());
            return 1;
        }
        const std::string text = json::writeCanonical(report) + "\n";
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    return 0;
}
