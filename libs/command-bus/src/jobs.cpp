// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/cmd/bus.h>
#include <algorithm>
#include <cmath>
#include <exception>

namespace supple::cmd {
namespace {
json::Value errorJson(const Error& error) {
    json::Value value = json::Value::object();
    value["code"] = error.code;
    value["message"] = error.message;
    value["path"] = error.path;
    value["hint"] = error.hint;
    if (!error.details.empty()) {
        value["details"] = json::Value::array();
        for (const auto& detail : error.details) value["details"].push_back(errorJson(detail));
    }
    return value;
}
}  // namespace

json::Value JobManager::snapshot(const Job& job) {
    json::Value value = json::Value::object();
    value["job"] = job.id;
    value["status"] = job.status;
    value["progress"] = job.progress;
    value["progress"]["fraction"] = job.fraction->load(std::memory_order_relaxed);
    if (job.status == "done" || !job.result.isNull()) value["result"] = job.result;
    if (job.status == "failed") value["error"] = errorJson(job.error);
    return value;
}
std::string JobManager::create() {
    std::lock_guard lock(mutex_);
    std::string id = "jb_" + std::to_string(next_++);
    Job job;
    job.id = id;
    job.progress["fraction"] = 0.0;
    job.progress["message"] = "";
    jobs_.emplace(id, std::move(job));
    order_.push_back(id);
    prune();
    return id;
}
bool JobManager::cancelled(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    return it == jobs_.end() || it->second.cancelRequested;
}
std::shared_ptr<const std::atomic<bool>> JobManager::cancellationFlag(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    return it == jobs_.end() ? nullptr : it->second.cancelFlag;
}
std::shared_ptr<std::atomic<double>> JobManager::progressFlag(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    return it == jobs_.end() ? nullptr : it->second.fraction;
}
Status JobManager::progress(std::string_view id, double fraction, std::string message,
                            json::Value extra) {
    std::unique_lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return Error("job.unknown", "unknown job", "/job", "use job.list");
    auto& job = it->second;
    if (job.status != "running") return Error("job.finished", "job is no longer running", "/job");
    if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
        return Error("job.progress", "fraction must be between 0 and 1", "/fraction");
    job.progress = extra.isObject() ? std::move(extra) : json::Value::object();
    job.progress["fraction"] = fraction;
    job.fraction->store(fraction, std::memory_order_relaxed);
    job.progress["message"] = std::move(message);
    bool queued = false;
    auto now = std::chrono::steady_clock::now();
    if (job.lastProgressEvent.time_since_epoch().count() == 0 ||
        now - job.lastProgressEvent >= std::chrono::milliseconds(500)) {
        events_.emplace_back("job.progress", snapshot(job));
        job.lastProgressEvent = now;
        queued = true;
    }
    cv_.notify_all();
    lock.unlock();
    if (queued && wake_) wake_();
    return {};
}
Status JobManager::complete(std::string_view id, json::Value result) {
    return completeWith(id, std::move(result), {});
}
Status JobManager::completeWith(std::string_view id, json::Value result,
                                const std::function<void()>& publish) {
    std::unique_lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return Error("job.unknown", "unknown job", "/job", "use job.list");
    if (it->second.status != "running")
        return Error("job.finished", "job is no longer running", "/job");
    if (it->second.cancelRequested) return Error("job.finished", "job was cancelled", "/job");
    if (publish) {
        try {
            publish();
        } catch (const std::exception& e) {
            return Error("job.publish", e.what(), "/job");
        } catch (...) {
            return Error("job.publish", "could not publish job result", "/job");
        }
    }
    it->second.status = "done";
    it->second.fraction->store(1.0, std::memory_order_relaxed);
    it->second.progress["fraction"] = 1.0;
    it->second.result = std::move(result);
    events_.emplace_back("job.done", snapshot(it->second));
    cv_.notify_all();
    prune();
    lock.unlock();
    if (wake_) wake_();
    return {};
}
Status JobManager::fail(std::string_view id, Error error, std::optional<json::Value> result) {
    std::unique_lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return Error("job.unknown", "unknown job", "/job", "use job.list");
    if (it->second.status != "running")
        return Error("job.finished", "job is no longer running", "/job");
    it->second.status = it->second.cancelRequested ? "cancelled" : "failed";
    it->second.error = std::move(error);
    if (result) it->second.result = std::move(*result);
    events_.emplace_back("job.done", snapshot(it->second));
    cv_.notify_all();
    prune();
    lock.unlock();
    if (wake_) wake_();
    return {};
}
Status JobManager::cancel(std::string_view id, std::optional<json::Value> result) {
    std::unique_lock lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return Error("job.unknown", "unknown job", "/job", "use job.list");
    if (it->second.status == "running") {
        it->second.cancelRequested = true;
        it->second.cancelFlag->store(true, std::memory_order_release);
        it->second.status = "cancelled";
    } else if (it->second.status != "cancelled" || !result) {
        return {};
    }
    if (result) it->second.result = std::move(*result);
    events_.emplace_back("job.done", snapshot(it->second));
    cv_.notify_all();
    prune();
    lock.unlock();
    if (wake_) wake_();
    return {};
}
Result<json::Value> JobManager::status(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) return Error("job.unknown", "unknown job", "/job", "use job.list");
    return snapshot(it->second);
}
Result<json::Value> JobManager::wait(std::string_view id, double timeoutSeconds) {
    if (!std::isfinite(timeoutSeconds) || timeoutSeconds < 0)
        return Error("job.timeout", "timeout_s must be a nonnegative number of seconds",
                     "/timeout_s", "0 returns at once, 30 waits for up to 30 seconds");
    // A longer request is clamped, not rejected: an agent that asks for 300 s
    // wants "as long as it takes", and losing the call teaches it nothing.
    const bool clamped = timeoutSeconds > kMaxWaitSeconds;
    if (clamped) timeoutSeconds = kMaxWaitSeconds;
    std::unique_lock lock(mutex_);
    auto key = std::string(id);
    auto it = jobs_.find(key);
    if (it == jobs_.end()) return Error("job.unknown", "unknown job", "/job", "use job.list");
    cv_.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), [&] {
        auto found = jobs_.find(key);
        return found == jobs_.end() || found->second.status != "running";
    });
    it = jobs_.find(key);
    if (it == jobs_.end()) return Error("job.unknown", "job expired", "/job", "use job.list");
    json::Value result = snapshot(it->second);
    if (clamped) result["timeout_clamped_s"] = kMaxWaitSeconds;
    return result;
}
json::Value JobManager::list() const {
    std::lock_guard lock(mutex_);
    json::Value result = json::Value::object();
    result["jobs"] = json::Value::array();
    for (const auto& id : order_) {
        auto it = jobs_.find(id);
        if (it != jobs_.end()) result["jobs"].push_back(snapshot(it->second));
    }
    return result;
}
std::vector<std::pair<std::string, json::Value>> JobManager::drainEvents() {
    std::lock_guard lock(mutex_);
    auto events = std::move(events_);
    events_.clear();
    return events;
}
void JobManager::prune() {
    std::size_t finished = 0;
    for (const auto& id : order_) {
        auto it = jobs_.find(id);
        if (it != jobs_.end() && it->second.status != "running") ++finished;
    }
    while (finished > 100) {
        auto it = std::find_if(order_.begin(), order_.end(), [&](const std::string& id) {
            auto found = jobs_.find(id);
            return found != jobs_.end() && found->second.status != "running";
        });
        if (it == order_.end()) break;
        jobs_.erase(*it);
        order_.erase(it);
        --finished;
    }
}
}  // namespace supple::cmd
