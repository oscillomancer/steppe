// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/peaks/peaks.h"
#include <supple/base/paths.h>
#include "supple/base/stop.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "supple/base/files.h"

namespace supple::peaks {
namespace {

std::int16_t q15(float v) {
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

float unq15(std::int16_t v) { return static_cast<float>(v) / 32767.0f; }

constexpr char kMagic[4] = {'S', 'P', 'P', 'K'};
constexpr std::uint32_t kFormat = 2;

template <class T>
void put(std::vector<std::uint8_t>& out, T v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof v);
}

template <class T>
bool get(std::span<const std::uint8_t>& in, T& v) {
    if (in.size() < sizeof v) return false;
    std::memcpy(&v, in.data(), sizeof v);
    in = in.subspan(sizeof v);
    return true;
}

std::string hexOf(const std::string& hash) {
    const auto colon = hash.find(':');
    std::string hex = colon == std::string::npos ? hash : hash.substr(colon + 1);
    // Only hex digits reach the file system.
    hex.erase(std::remove_if(hex.begin(), hex.end(),
                             [](char c) { return !std::isxdigit(static_cast<unsigned char>(c)); }),
              hex.end());
    return hex;
}

std::string fileTimeKey(std::filesystem::file_time_type time) {
    // libc++ can use a 128-bit filesystem clock. Preserve all its ticks,
    // including the most negative value, without narrowing to long long.
    auto ticks = time.time_since_epoch().count();
    const bool negative = ticks < 0;
    std::string key;
    do {
        const auto digit = ticks % 10;
        key.push_back(static_cast<char>('0' + (digit < 0 ? -digit : digit)));
        ticks /= 10;
    } while (ticks != 0);
    if (negative) key.push_back('-');
    std::reverse(key.begin(), key.end());
    return key;
}

// The in-memory key: the content hash, or path + size + mtime when the
// model has none yet (MediaSource::hash is not populated everywhere).
std::string keyOf(const std::string& hash, const std::filesystem::path& media) {
    if (!hash.empty()) return hash;
    std::error_code sizeError, timeError;
    const auto size = std::filesystem::file_size(media, sizeError);
    const auto mtime = std::filesystem::last_write_time(media, timeError);
    return "path:" + media.lexically_normal().string() + ":" +
           (sizeError ? "missing" : std::to_string(size)) + ":" +
           (timeError ? "missing" : fileTimeKey(mtime));
}

// The cache file name: the hash's hex, or a digest of path, size and
// modification time (so an edited file is rebuilt).
std::string cacheName(const std::string& hash, const std::filesystem::path& media) {
    if (!hash.empty() && !hexOf(hash).empty()) return hexOf(hash) + ".peaks";
    std::error_code ec;
    const auto size = std::filesystem::file_size(media, ec);
    const auto mtime = std::filesystem::last_write_time(media, ec).time_since_epoch().count();
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a 64
    auto mix = [&](const void* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            h ^= static_cast<const std::uint8_t*>(data)[i];
            h *= 1099511628211ull;
        }
    };
    const std::string path = media.lexically_normal().string();
    mix(path.data(), path.size());
    mix(&size, sizeof size);
    mix(&mtime, sizeof mtime);
    char name[32];
    std::snprintf(name, sizeof name, "p-%016llx.peaks", static_cast<unsigned long long>(h));
    return name;
}

}  // namespace

Pyramid Pyramid::build(const audio::AudioData& audio) {
    Pyramid p;
    p.channels_ = static_cast<int>(audio.channelCount());
    p.sampleRate_ = audio.sampleRate;
    p.frames_ = static_cast<std::int64_t>(audio.frames());
    const auto channels = static_cast<std::size_t>(p.channels_);
    const std::size_t frames = audio.frames();

    // The finest level from the samples; RMS kept as mean squares until the
    // level is final so coarser levels combine exactly.
    Level fine;
    fine.samplesPerBucket = kSamplesPerBucket[0];
    fine.count = (frames + fine.samplesPerBucket - 1) / fine.samplesPerBucket;
    fine.buckets.resize(fine.count * channels);
    std::vector<double> meanSquares(fine.count * channels);
    for (std::size_t c = 0; c < channels; ++c) {
        const float* x = audio.channels[c].data();
        for (std::size_t b = 0; b < fine.count; ++b) {
            const std::size_t from = b * fine.samplesPerBucket;
            const std::size_t to = std::min(frames, from + fine.samplesPerBucket);
            float lo = x[from], hi = x[from];
            double sq = 0;
            for (std::size_t i = from; i < to; ++i) {
                lo = std::min(lo, x[i]);
                hi = std::max(hi, x[i]);
                sq += static_cast<double>(x[i]) * static_cast<double>(x[i]);
            }
            meanSquares[b * channels + c] = sq / static_cast<double>(to - from);
            fine.buckets[b * channels + c] = {q15(lo), q15(hi), 0};
        }
    }
    auto finish = [&](Level& level, const std::vector<double>& ms) {
        for (std::size_t i = 0; i < level.buckets.size(); ++i)
            level.buckets[i].rms = q15(static_cast<float>(std::sqrt(ms[i])));
    };
    finish(fine, meanSquares);
    std::vector<double> prevSquares = std::move(meanSquares);
    p.levels_.push_back(std::move(fine));

    // Coarser levels: each bucket combines four of the level below.
    for (std::size_t l = 1; l < kSamplesPerBucket.size(); ++l) {
        const Level& below = p.levels_.back();
        Level level;
        level.samplesPerBucket = kSamplesPerBucket[l];
        const std::size_t factor = kSamplesPerBucket[l] / kSamplesPerBucket[l - 1];
        level.count = (below.count + factor - 1) / factor;
        level.buckets.resize(level.count * channels);
        std::vector<double> squares(level.count * channels);
        for (std::size_t b = 0; b < level.count; ++b)
            for (std::size_t c = 0; c < channels; ++c) {
                const std::size_t from = b * factor, to = std::min(below.count, from + factor);
                Bucket out = below.buckets[from * channels + c];
                double sq = 0;
                std::size_t samples = 0;
                for (std::size_t i = from; i < to; ++i) {
                    const Bucket& in = below.buckets[i * channels + c];
                    out.min = std::min(out.min, in.min);
                    out.max = std::max(out.max, in.max);
                    const auto weight = std::min<std::size_t>(below.samplesPerBucket,
                                                              frames - i * below.samplesPerBucket);
                    sq += prevSquares[i * channels + c] * static_cast<double>(weight);
                    samples += weight;
                }
                squares[b * channels + c] = sq / static_cast<double>(samples);
                level.buckets[b * channels + c] = out;
            }
        finish(level, squares);
        prevSquares = std::move(squares);
        p.levels_.push_back(std::move(level));
    }
    return p;
}

const Level& Pyramid::pick(double samplesPerColumn) const {
    const Level* best = &levels_.front();
    for (const Level& l : levels_)
        if (static_cast<double>(l.samplesPerBucket) <= samplesPerColumn) best = &l;
    return *best;
}

Span Pyramid::range(const Level& level, int channel, double from, double to) const {
    if (level.count == 0 || channel < 0 || channel >= channels_) return {};
    const double spb = level.samplesPerBucket;
    const auto last = static_cast<std::int64_t>(level.count) - 1;
    auto first = static_cast<std::int64_t>(std::floor(from / spb));
    auto end = static_cast<std::int64_t>(std::ceil(to / spb));
    if (end <= first) end = first + 1;
    if (first > last || end <= 0) return {};
    first = std::max<std::int64_t>(first, 0);
    end = std::min(end, last + 1);
    const auto channels = static_cast<std::size_t>(channels_);
    const auto c = static_cast<std::size_t>(channel);
    Bucket b = level.buckets[static_cast<std::size_t>(first) * channels + c];
    double sq = 0;
    double samples = 0;
    for (auto i = first; i < end; ++i) {
        const Bucket& in = level.buckets[static_cast<std::size_t>(i) * channels + c];
        b.min = std::min(b.min, in.min);
        b.max = std::max(b.max, in.max);
        const double weight =
            std::min(spb, static_cast<double>(frames_) - static_cast<double>(i) * spb);
        sq += static_cast<double>(unq15(in.rms)) * static_cast<double>(unq15(in.rms)) * weight;
        samples += weight;
    }
    return {unq15(b.min), unq15(b.max), static_cast<float>(std::sqrt(sq / samples))};
}

std::vector<std::uint8_t> Pyramid::serialize() const {
    std::vector<std::uint8_t> out(kMagic, kMagic + 4);
    put(out, kFormat);
    put(out, static_cast<std::uint32_t>(channels_));
    put(out, sampleRate_);
    put(out, frames_);
    put(out, static_cast<std::uint32_t>(levels_.size()));
    for (const Level& l : levels_) {
        put(out, l.samplesPerBucket);
        put(out, static_cast<std::uint64_t>(l.count));
        const auto* p = reinterpret_cast<const std::uint8_t*>(l.buckets.data());
        if (!l.buckets.empty()) out.insert(out.end(), p, p + l.buckets.size() * sizeof(Bucket));
    }
    return out;
}

Result<Pyramid> Pyramid::deserialize(std::span<const std::uint8_t> in) {
    auto bad = [] { return Error("peaks.cache", "not a peak cache file of this version"); };
    if (in.size() < 4 || std::memcmp(in.data(), kMagic, 4) != 0) return bad();
    in = in.subspan(4);
    std::uint32_t format = 0, channels = 0, levels = 0;
    Pyramid p;
    if (!get(in, format) || format != kFormat || !get(in, channels) || channels == 0 ||
        channels > 64 || !get(in, p.sampleRate_) || !get(in, p.frames_) || !get(in, levels) ||
        levels != kSamplesPerBucket.size())
        return bad();
    if (!std::isfinite(p.sampleRate_) || p.sampleRate_ <= 0 || p.frames_ < 0) return bad();
    p.channels_ = static_cast<int>(channels);
    for (std::uint32_t i = 0; i < levels; ++i) {
        Level l;
        std::uint64_t count = 0;
        if (!get(in, l.samplesPerBucket) || l.samplesPerBucket != kSamplesPerBucket[i] ||
            !get(in, count))
            return bad();
        const auto frames = static_cast<std::uint64_t>(p.frames_);
        if (count != frames / l.samplesPerBucket + (frames % l.samplesPerBucket != 0)) return bad();
        const std::uint64_t bytes = count * channels * sizeof(Bucket);
        if (count > (std::uint64_t{1} << 34) || in.size() < bytes) return bad();
        l.count = static_cast<std::size_t>(count);
        l.buckets.resize(l.count * channels);
        // A level of an empty file has no buckets, and memcpy takes no null.
        if (bytes != 0) std::memcpy(l.buckets.data(), in.data(), static_cast<std::size_t>(bytes));
        in = in.subspan(static_cast<std::size_t>(bytes));
        p.levels_.push_back(std::move(l));
    }
    if (!in.empty()) return bad();
    return p;
}

std::filesystem::path defaultCacheDir(std::string_view slug) {
    if (const auto root = paths::cache(); !root.empty()) return root / slug / "peaks";
    return std::filesystem::temp_directory_path() / slug / "peaks";
}

PeakService::PeakService(std::filesystem::path cacheDir, Post post)
    : cacheDir_(std::move(cacheDir)),
      post_(std::move(post)),
      worker_([this](supple::StopToken stop) { run(stop); }) {}

PeakService::~PeakService() {
    {
        std::lock_guard lock(mutex_);
        worker_.request_stop();
    }
    wake_.notify_all();
}

std::string PeakService::key(const std::string& hash, const std::filesystem::path& media) const {
    if (!hash.empty()) return hash;
    const auto now = std::chrono::steady_clock::now();
    const std::string path = media.lexically_normal().string();
    {
        std::lock_guard lock(mutex_);
        if (auto it = pathKeys_.find(path);
            it != pathKeys_.end() && now - it->second.second < std::chrono::seconds(2))
            return it->second.first;
    }
    std::string fresh = keyOf(hash, media);  // stat outside the lock
    std::lock_guard lock(mutex_);
    pathKeys_[path] = {fresh, now};
    return fresh;
}

void PeakService::revalidate() {
    std::lock_guard lock(mutex_);
    pathKeys_.clear();
}

namespace {
// The path as a hold compares it: absolute and normal, symlinks resolved where
// the file system has them (a scratch folder under a linked home, macOS's /var).
std::filesystem::path held(const std::filesystem::path& p) {
    std::error_code ec;
    auto out = std::filesystem::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : out;
}
bool within(const std::filesystem::path& file, const std::filesystem::path& dir) {
    const auto rel = file.lexically_relative(dir);
    return !rel.empty() && *rel.begin() != "..";
}
}  // namespace

bool PeakService::heldLocked(const std::filesystem::path& media) const {
    return std::any_of(held_.begin(), held_.end(),
                       [&](const std::filesystem::path& dir) { return within(media, dir); });
}

bool PeakService::holding() const {
    std::lock_guard lock(mutex_);
    return !held_.empty();
}

std::function<void()> PeakService::holdUnder(const std::filesystem::path& dir) {
    const auto at = held(dir);
    std::unique_lock lock(mutex_);
    held_.push_back(at);
    idle_.wait(lock, [&] { return reading_.empty() || !within(reading_, at); });
    return [this, at, alive = std::weak_ptr<int>(alive_)] {
        if (!alive.lock()) return;
        std::vector<std::string> again;
        {
            std::lock_guard guard(mutex_);
            const auto it = std::find(held_.begin(), held_.end(), at);
            if (it != held_.end()) held_.erase(it);
            if (held_.empty()) again.swap(stopped_);
        }
        // The owner asks for them again (they may be somewhere else now).
        if (post_)
            for (auto& key : again)
                post_([this, alive, key = std::move(key)] {
                    if (alive.lock() && onReady) onReady(key);
                });
    };
}

std::shared_ptr<const Pyramid> PeakService::get(const std::string& hash,
                                                const std::filesystem::path& media) {
    const std::string key = this->key(hash, media);
    const bool holding = [&] {
        std::lock_guard lock(mutex_);
        return !held_.empty();
    }();
    const auto where = holding ? held(media) : std::filesystem::path{};
    std::lock_guard lock(mutex_);
    if (auto it = ready_.find(key); it != ready_.end()) return it->second;
    if (failed_.count(key) || pending_.count(key)) return nullptr;
    if (!held_.empty() && heldLocked(holding ? where : held(media))) {
        stopped_.push_back(key);  // asked for again when the hold ends
        return nullptr;
    }
    pending_[key] = true;
    queue_.push_back({key, hash, media});
    wake_.notify_one();
    return nullptr;
}

std::optional<Error> PeakService::failure(const std::string& hash,
                                          const std::filesystem::path& media) const {
    const std::string k = key(hash, media);
    std::lock_guard lock(mutex_);
    auto it = failed_.find(k);
    return it == failed_.end() ? std::nullopt : std::optional<Error>(it->second);
}

PeakService::Stats PeakService::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void PeakService::run(supple::StopToken stop) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop.stop_requested() || !queue_.empty(); });
            if (stop.stop_requested()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        const auto& [key, hash, media] = job;
        const std::filesystem::path where = held(media);
        {
            std::lock_guard lock(mutex_);
            if (heldLocked(where)) {  // not opened while held: asked for again after
                pending_.erase(key);
                stopped_.push_back(key);
                continue;
            }
            reading_ = where;
        }
        struct Done {
            PeakService& service;
            ~Done() {
                {
                    std::lock_guard lock(service.mutex_);
                    service.reading_.clear();
                }
                service.idle_.notify_all();
            }
        } done{*this};
        const std::filesystem::path cached = cacheDir_ / cacheName(hash, media);
        std::shared_ptr<const Pyramid> pyramid;
        std::optional<Error> error;
        bool loaded = false;
        if (auto bytes = readFile(cached)) {
            if (auto p = Pyramid::deserialize(*bytes)) {
                pyramid = std::make_shared<const Pyramid>(std::move(*p));
                loaded = true;
            }
        }
        bool stopped = false;
        if (!pyramid) {
            // A chunk at a time, so a hold (holdUnder) can stop the read and
            // close the file between two chunks; whole-file reads for the
            // formats only readAudio knows (none of them a recording).
            audio::HeadOptions read;
            read.stop = [&] {
                if (readHook) readHook(media);
                std::lock_guard lock(mutex_);
                stopped = stop.stop_requested() || heldLocked(where);
                return stopped;
            };
            auto audio = audio::readAudioHead(media, read);
            if (!audio && audio.error().code == "audio.format") audio = audio::readAudio(media);
            if (!audio && stopped) {
                std::lock_guard lock(mutex_);
                pending_.erase(key);
                stopped_.push_back(key);
                continue;  // not a failure: asked for again when the hold ends
            }
            if (!audio) {
                error = audio.error();
            } else {
                auto built = std::make_shared<const Pyramid>(Pyramid::build(*audio));
                std::error_code ec;
                std::filesystem::create_directories(cacheDir_, ec);
                (void)writeFileAtomic(
                    cached, std::span<const std::uint8_t>(built->serialize()));  // best effort
                pyramid = std::move(built);
            }
        }
        {
            std::lock_guard lock(mutex_);
            pending_.erase(key);
            if (pyramid) {
                ready_[key] = pyramid;
                ++(loaded ? stats_.loaded : stats_.built);
            } else {
                failed_.emplace(key, *error);
                ++stats_.failed;
            }
        }
        if (post_)
            post_([this, alive = std::weak_ptr<int>(alive_), readyKey = key] {
                if (alive.lock() && onReady) onReady(readyKey);
            });
    }
}

}  // namespace supple::peaks
