// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>
#include "test_support.h"

#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/peaks/peaks.h"

using namespace supple;

namespace {

audio::AudioData tone(std::size_t frames, double rate = 48000.0) {
    audio::AudioData a;
    a.sampleRate = rate;
    a.channels.assign(2, std::vector<float>(frames));
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        a.channels[0][i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 440 * t));
        // Right: silence, then a half-scale DC step in the second half.
        a.channels[1][i] = i < frames / 2 ? 0.0f : 0.25f;
    }
    return a;
}

std::filesystem::path scratch(const char* name) {
    auto dir = std::filesystem::temp_directory_path() /
               ("supple-peaks-test-" + std::to_string(testProcessId())) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("a pyramid has every level with exact min, max and RMS") {
    const auto audio = tone(100000);
    const auto p = peaks::Pyramid::build(audio);
    REQUIRE(p.levels().size() == peaks::kSamplesPerBucket.size());
    CHECK(p.channels() == 2);
    CHECK(p.frames() == 100000);
    CHECK(p.levels()[0].count == (100000 + 63) / 64);
    CHECK(p.levels()[4].count == (100000 + 16383) / 16384);
    // The whole file from the coarsest level.
    const auto& top = p.levels().back();
    const auto left = p.range(top, 0, 0, 100000);
    CHECK(left.max == doctest::Approx(0.5).epsilon(0.001));
    CHECK(left.min == doctest::Approx(-0.5).epsilon(0.001));
    CHECK(left.rms == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
    // The right channel's first half is silent, its second a 0.25 step.
    const auto& fine = p.levels().front();
    const auto quiet = p.range(fine, 1, 0, 49000);
    CHECK(quiet.min == 0.0f);
    CHECK(quiet.max == 0.0f);
    const auto step = p.range(fine, 1, 51000, 99000);
    CHECK(step.min == doctest::Approx(0.25).epsilon(0.001));
    CHECK(step.rms == doctest::Approx(0.25).epsilon(0.001));
    // Coarse levels agree with fine ones over the same span.
    const auto viaFine = p.range(fine, 0, 0, 65536);
    const auto viaCoarse = p.range(p.levels()[3], 0, 0, 65536);
    CHECK(viaFine.max == viaCoarse.max);
    CHECK(viaFine.rms == doctest::Approx(static_cast<double>(viaCoarse.rms)).epsilon(0.001));
}

TEST_CASE("pick chooses the coarsest level with at least one bucket per column") {
    const auto p = peaks::Pyramid::build(tone(50000));
    CHECK(p.pick(10).samplesPerBucket == 64);  // zoomed past the finest level
    CHECK(p.pick(64).samplesPerBucket == 64);
    CHECK(p.pick(1000).samplesPerBucket == 256);
    CHECK(p.pick(5000).samplesPerBucket == 4096);
    CHECK(p.pick(1e9).samplesPerBucket == 16384);
    // Out-of-range spans are empty, not errors.
    const auto none = p.range(p.levels()[0], 0, 1e7, 2e7);
    CHECK(none.max == 0.0f);
    CHECK(p.range(p.levels()[0], 5, 0, 100).max == 0.0f);
}

TEST_CASE("pyramids serialize exactly and reject foreign or truncated files") {
    const auto p = peaks::Pyramid::build(tone(30000));
    const auto bytes = p.serialize();
    auto back = peaks::Pyramid::deserialize(bytes);
    REQUIRE(back);
    CHECK(back->frames() == p.frames());
    CHECK(back->serialize() == bytes);
    auto old = bytes;
    old[4] = 1;
    old[5] = old[6] = old[7] = 0;
    CHECK_FALSE(peaks::Pyramid::deserialize(old));
    auto truncated = bytes;
    truncated.resize(truncated.size() - 7);
    CHECK_FALSE(peaks::Pyramid::deserialize(truncated));
    std::vector<std::uint8_t> junk(64, 0x41);
    CHECK_FALSE(peaks::Pyramid::deserialize(junk));
}

TEST_CASE("the peak service builds once off-thread, caches on disk and loads the cache") {
    const auto dir = scratch("service");
    const auto wav = dir / "tone.wav";
    REQUIRE(writeFileAtomic(wav, std::span<const std::uint8_t>(*audio::encodeWav(tone(48000)))));
    const std::string hash = "sha256:00ff00ff";

    // The owner's loop: posted tasks run when the test drains them.
    std::mutex m;
    std::vector<std::function<void()>> posted;
    auto post = [&](std::function<void()> f) {
        std::lock_guard lock(m);
        posted.push_back(std::move(f));
    };
    auto drainUntil = [&](auto done, int rounds = 500) {
        for (int i = 0; i < rounds && !done(); ++i) {
            std::vector<std::function<void()>> now;
            {
                std::lock_guard lock(m);
                now.swap(posted);
            }
            for (auto& f : now) f();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done();
    };

    {
        peaks::PeakService service(dir / "cache", post);
        std::vector<std::string> ready;
        service.onReady = [&](const std::string& h) { ready.push_back(h); };
        CHECK(service.get(hash, wav) == nullptr);  // queued, never blocks
        service.get(hash, wav);  // asked again, while queued or once built: built once
        REQUIRE(drainUntil([&] { return !ready.empty(); }));
        CHECK(ready == std::vector<std::string>{hash});
        auto p = service.get(hash, wav);
        REQUIRE(p);
        CHECK(p->frames() == 48000);
        CHECK(service.stats().built == 1);
        CHECK(std::filesystem::exists(dir / "cache" / "00ff00ff.peaks"));
    }
    {
        peaks::PeakService service(dir / "cache", post);
        bool ready = false;
        service.onReady = [&](const std::string&) { ready = true; };
        CHECK(service.get(hash, wav) == nullptr);
        REQUIRE(drainUntil([&] { return ready; }));
        CHECK(service.stats().loaded == 1);  // from the cache, not rebuilt
        CHECK(service.stats().built == 0);
    }
    {
        peaks::PeakService service(dir / "cache", post);
        bool ready = false;
        service.onReady = [&](const std::string&) { ready = true; };
        CHECK(service.get("sha256:abcd", dir / "missing.wav") == nullptr);
        REQUIRE(drainUntil([&] { return ready; }));
        CHECK(service.failure("sha256:abcd", dir / "missing.wav").has_value());
        CHECK(service.get("sha256:abcd", dir / "missing.wav") == nullptr);  // no retry loop
        CHECK(service.stats().failed == 1);
    }
    // Without a content hash the path keys it, and distinct files stay distinct.
    {
        const auto other = dir / "other.wav";
        REQUIRE(
            writeFileAtomic(other, std::span<const std::uint8_t>(*audio::encodeWav(tone(24000)))));
        peaks::PeakService service(dir / "cache", post);
        int ready = 0;
        service.onReady = [&](const std::string& key) {
            CHECK(key.starts_with("path:"));
            ++ready;
        };
        service.get("", wav);
        service.get("", other);
        REQUIRE(drainUntil([&] { return ready == 2; }));
        REQUIRE(service.get("", wav));
        REQUIRE(service.get("", other));
        CHECK(service.get("", wav)->frames() == 48000);
        CHECK(service.get("", other)->frames() == 24000);
        int named = 0;
        for (const auto& f : std::filesystem::directory_iterator(dir / "cache"))
            named += f.path().filename().string().starts_with("p-");
        CHECK(named == 2);
    }
    // A notification still queued after its service is gone is dropped safely.
    {
        auto service = std::make_unique<peaks::PeakService>(dir / "cache", post);
        service->onReady = [](const std::string&) { FAIL("called after destruction"); };
        service->get(hash, wav);
        for (int i = 0; i < 200 && service->stats().loaded == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        service.reset();
        drainUntil([] { return false; }, 5);
    }
    std::filesystem::remove_all(dir.parent_path());
}

namespace {
// The files this process holds open whose name ends with `name`: Linux only
// (/proc), where a removed file that is still open is listed too.
std::vector<std::string> openFilesNamed([[maybe_unused]] const std::string& name) {
    std::vector<std::string> out;
#if defined(__linux__)
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code link;
        const auto target = std::filesystem::read_symlink(it->path(), link);
        std::string text = target.string();
        constexpr std::string_view gone = " (deleted)";
        if (text.ends_with(gone)) text.resize(text.size() - gone.size());
        if (!link && text.ends_with(name)) out.push_back(target.string());
    }
#endif
    return out;
}
}  // namespace

// A first save moves a fresh recording out of the scratch folder and removes
// it there, which Windows refuses while the waveform worker has it open: a
// hold makes the worker let go between two chunks and open nothing there.
TEST_CASE("a hold makes the peak worker let go of the files under a folder until it ends") {
    const auto dir = scratch("hold");
    const auto media = dir / "media";
    std::filesystem::create_directories(media);
    const auto wav = media / "Vocal-1.wav";  // several 16384-frame chunks
    REQUIRE(writeFileAtomic(wav, std::span<const std::uint8_t>(*audio::encodeWav(tone(100000)))));
    std::mutex m;
    std::vector<std::function<void()>> posted;
    auto post = [&](std::function<void()> f) {
        std::lock_guard lock(m);
        posted.push_back(std::move(f));
    };
    auto drain = [&] {
        std::vector<std::function<void()>> now;
        {
            std::lock_guard lock(m);
            now.swap(posted);
        }
        for (auto& f : now) f();
    };
    peaks::PeakService service(dir / "cache", post);
    std::vector<std::string> ready;
    service.onReady = [&](const std::string& key) { ready.push_back(key); };
    // The worker stops at its first chunk boundary until the hold is in place,
    // then lingers there with the file open: a hold that returned before the
    // worker let go would find it still open. (The pause decides nothing when
    // the hold waits, as it must.)
    std::atomic<bool> entered{false}, go{false};
    service.readHook = [&](const std::filesystem::path&) {
        if (!entered.exchange(true)) {
            entered.notify_all();
            go.wait(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    CHECK(service.get("", wav) == nullptr);  // a fresh recording: no hash yet
    entered.wait(false);
    std::function<void()> release;
    std::thread holder([&] { release = service.holdUnder(media); });
    while (!service.holding()) std::this_thread::yield();
    go.store(true);
    go.notify_all();
    holder.join();  // returns once the worker has let go
    // Nothing of the file is open: it can be removed, as a first save does
    // after copying it, and the stopped read is no failure.
    CHECK(openFilesNamed("Vocal-1.wav").empty());
    const auto moved = dir / "song" / "Vocal-1.wav";
    std::filesystem::create_directories(moved.parent_path());
    std::filesystem::copy_file(wav, moved);
    std::error_code removed;
    std::filesystem::remove(wav, removed);
    CHECK_FALSE(removed);
    CHECK(service.stats().built == 0);
    CHECK(service.stats().failed == 0);
    CHECK_FALSE(service.failure("", wav));
    // While held, nothing under the folder is queued.
    REQUIRE(writeFileAtomic(media / "Vocal-2.wav",
                            std::span<const std::uint8_t>(*audio::encodeWav(tone(4800)))));
    CHECK(service.get("", media / "Vocal-2.wav") == nullptr);
    // The hold ends: the owner hears of both, and asks again where they are now.
    release();
    for (int i = 0; i < 500 && ready.size() < 2; ++i) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(ready.size() == 2);
    CHECK(service.stats().built == 0);
    std::shared_ptr<const peaks::Pyramid> pyramid;
    for (int i = 0; i < 500 && !pyramid; ++i) {
        pyramid = service.get("", moved);
        if (!pyramid) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(pyramid);
    CHECK(pyramid->frames() == 100000);
    CHECK(service.stats().built == 1);
}

TEST_CASE("partial peak buckets weight RMS by the number of samples") {
    audio::AudioData data;
    data.sampleRate = 48000;
    data.channels = {std::vector<float>(65, 0.0f)};
    data.channels.front().back() = 1.0f;
    const auto pyramid = peaks::Pyramid::build(data);
    for (const auto& level : pyramid.levels())
        CHECK(pyramid.range(level, 0, 0, 65).rms ==
              doctest::Approx(1.0 / std::sqrt(65.0)).epsilon(0.00001));
}

TEST_CASE("peak cache rejects inconsistent metadata") {
    const auto bytes = peaks::Pyramid::build(tone(65)).serialize();
    auto check = [&](std::size_t offset, const auto& value) {
        auto bad = bytes;
        std::memcpy(bad.data() + offset, &value, sizeof value);
        CHECK_FALSE(peaks::Pyramid::deserialize(bad));
    };
    check(12, 0.0);  // sample rate
    check(12, std::numeric_limits<double>::quiet_NaN());
    check(20, std::int64_t{-1});  // frame count
    check(36, std::uint64_t{1});  // first level needs two buckets
    audio::AudioData empty;
    empty.sampleRate = 48000;
    empty.channels.resize(1);
    const auto serialized = peaks::Pyramid::build(empty).serialize();
    auto back = peaks::Pyramid::deserialize(serialized);
    REQUIRE(back);
    CHECK(back->frames() == 0);
    CHECK(back->serialize() == serialized);
}

TEST_CASE("unhashed peaks rebuild replaced files and retry after a missing file appears") {
    const auto dir = scratch("revisions");
    const auto wav = dir / "tone.wav";
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::function<void()>> posted;
    peaks::PeakService service(dir / "cache", [&](auto fn) {
        std::lock_guard lock(mutex);
        posted.push_back(std::move(fn));
        ready.notify_one();
    });
    auto drain = [&] {
        std::vector<std::function<void()>> tasks;
        {
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return !posted.empty(); });
            tasks.swap(posted);
        }
        for (auto& task : tasks) task();
    };
    CHECK_FALSE(service.get("", wav));
    drain();
    CHECK(service.failure("", wav).has_value());
    REQUIRE(audio::writeWav(wav, tone(24000)));
    // Sizes and mtimes are remembered for two seconds; the app revalidates on
    // every session version (an import, a recording) and the test does here.
    CHECK(service.failure("", wav).has_value());
    service.revalidate();
    CHECK_FALSE(service.get("", wav));
    drain();
    REQUIRE(service.get("", wav));
    CHECK(service.get("", wav)->frames() == 24000);
    REQUIRE(audio::writeWav(wav, tone(48000)));
    service.revalidate();
    CHECK_FALSE(service.get("", wav));
    drain();
    REQUIRE(service.get("", wav));
    CHECK(service.get("", wav)->frames() == 48000);
    CHECK(service.stats().failed == 1);
    CHECK(service.stats().built == 2);
    std::filesystem::remove_all(dir);
}
