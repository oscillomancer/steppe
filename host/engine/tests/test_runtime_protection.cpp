// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>
#include "fake_jack.h"
#include "supple/base/files.h"
#include "supple/engine/jack.h"

#include <cerrno>

#include "runtime_protection.h"
#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;

namespace {
struct MemoryOps {
    std::shared_ptr<RuntimeProtectionOps> ops = std::make_shared<RuntimeProtectionOps>();
    int locks = 0, unlocks = 0, lockError = 0, unlockError = 0, flags = 0;
    MemoryOps() {
        ops->lockMemory = [this](int f) {
            ++locks;
            flags = f;
            errno = lockError;
            return lockError ? -1 : 0;
        };
        ops->unlockMemory = [this] {
            ++unlocks;
            errno = unlockError;
            return unlockError ? -1 : 0;
        };
        ops->memoryLimit = [](rlimit* limit) {
            limit->rlim_cur = 4096;
            return 0;
        };
        // The process's THP mode, as the kernel keeps it: 0 none, 1 all, 3
        // all but advised regions.
        ops->thpDisable = [this](unsigned long disable, unsigned long f) {
            thpCalls.emplace_back(disable, f);
            const int e = disable && f ? thpFlagsError : disable ? thpError : thpReleaseError;
            errno = e;
            if (e) return -1;
            thpMode = disable ? (f ? 3 : 1) : 0;
            return 0;
        };
        ops->thpMode = [this] {
            if (thpQueryError) {
                errno = thpQueryError;
                return -1;
            }
            return thpMode;
        };
    }
    std::vector<std::pair<unsigned long, unsigned long>> thpCalls;
    int thpFlagsError = 0, thpError = 0, thpReleaseError = 0, thpMode = 0, thpQueryError = 0;
};
}  // namespace

TEST_CASE("the process stays out of transparent huge pages while the engine runs") {
    using Call = std::pair<unsigned long, unsigned long>;
    SUBCASE("advised regions keep them where the kernel takes the flag") {
        MemoryOps fake;
        detail::RuntimeProtection controls(fake.ops);
        CHECK_FALSE(controls.thpOptOut().active);
        controls.start();
        CHECK(fake.thpCalls == std::vector<Call>{{1, 2}});  // PR_THP_DISABLE_EXCEPT_ADVISED
        CHECK(controls.thpOptOut().active);
        CHECK(controls.thpOptOut().advisedOnly);
        CHECK_FALSE(controls.thpOptOut().inherited);
        controls.start();  // once
        CHECK(fake.thpCalls.size() == 1);
        controls.stop();
        REQUIRE(fake.thpCalls.size() == 2);
        CHECK(fake.thpCalls.back() == Call{0, 0});
        CHECK(fake.thpMode == 0);
        CHECK_FALSE(controls.thpOptOut().active);
        CHECK_FALSE(controls.thpOptOut().advisedOnly);
    }
    SUBCASE("an inherited setting stays as it was, stricter or the same, and is not undone") {
        // Round 21: whatever launched the process, or a plug-in, may have kept
        // it out entirely (1) or as advised-only (3). 0->3->0 is the engine's
        // own; 1 and 3 must end as they began.
        for (const int inherited : {1, 3}) {
            CAPTURE(inherited);
            MemoryOps fake;
            fake.thpMode = inherited;
            detail::RuntimeProtection controls(fake.ops);
            controls.start();
            CHECK(fake.thpCalls.empty());
            CHECK(fake.thpMode == inherited);
            CHECK(controls.thpOptOut().active);
            CHECK(controls.thpOptOut().inherited);
            CHECK(controls.thpOptOut().advisedOnly == (inherited == 3));
            controls.stop();
            CHECK(fake.thpCalls.empty());
            CHECK(fake.thpMode == inherited);
            CHECK_FALSE(controls.thpOptOut().active);
        }
    }
    SUBCASE("with the kernel's own prctl, each starting mode ends as it began") {
        // This test process's own setting only, restored after.
        const int before = ::prctl(PR_GET_THP_DISABLE, 0UL, 0UL, 0UL, 0UL);
        REQUIRE(before >= 0);
        for (const unsigned long flags : {~0UL, 0UL, 2UL}) {  // none, all, advised-only
            CAPTURE(flags);
            if (flags == ~0UL)
                REQUIRE(::prctl(PR_SET_THP_DISABLE, 0UL, 0UL, 0UL, 0UL) == 0);
            else if (::prctl(PR_SET_THP_DISABLE, 1UL, flags, 0UL, 0UL) != 0)
                continue;  // a kernel before 6.18 has no advised-only mode
            const int start = ::prctl(PR_GET_THP_DISABLE, 0UL, 0UL, 0UL, 0UL);
            detail::RuntimeProtection controls(std::make_shared<RuntimeProtectionOps>());
            controls.start();
            CHECK(controls.thpOptOut().active);
            CHECK(::prctl(PR_GET_THP_DISABLE, 0UL, 0UL, 0UL, 0UL) != 0);
            controls.stop();
            CHECK(::prctl(PR_GET_THP_DISABLE, 0UL, 0UL, 0UL, 0UL) == start);
        }
        (void)::prctl(PR_SET_THP_DISABLE, before ? 1UL : 0UL, before == 3 ? 2UL : 0UL, 0UL, 0UL);
    }
    SUBCASE("a setting changed while the engine ran is not the engine's to undo") {
        MemoryOps fake;
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        REQUIRE(fake.thpCalls.size() == 1);
        fake.thpMode = 1;  // a plug-in keeps the process out entirely
        controls.stop();
        CHECK(fake.thpCalls.size() == 1);
        CHECK(fake.thpMode == 1);
        CHECK_FALSE(controls.thpOptOut().active);
    }
    SUBCASE("a refused mode query at stop is reported, and a later stop releases") {
        // Round 23: a failed PR_GET_THP_DISABLE at stop cleared ownership, so
        // the process stayed opted out with nothing left to undo it.
        MemoryOps fake;
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        REQUIRE(fake.thpMode == 3);
        fake.thpQueryError = EPERM;
        controls.stop();
        CHECK(fake.thpMode == 3);
        CHECK(controls.thpOptOut().active);  // still kept out, still the engine's
        REQUIRE(controls.thpOptOut().error);
        CHECK(controls.thpOptOut().error->errnoValue == EPERM);
        fake.thpQueryError = 0;
        controls.stop();
        CHECK(fake.thpMode == 0);
        CHECK_FALSE(controls.thpOptOut().active);
    }
    SUBCASE("a refused release is reported, and the engine still owns it") {
        MemoryOps fake;
        fake.thpReleaseError = EPERM;
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        controls.stop();
        REQUIRE(fake.thpCalls.size() == 2);
        CHECK(fake.thpMode == 3);
        CHECK(controls.thpOptOut().active);  // it is still kept out
        REQUIRE(controls.thpOptOut().error);
        CHECK(controls.thpOptOut().error->errnoValue == EPERM);
        fake.thpReleaseError = 0;
        controls.start();  // still active: nothing set again
        CHECK(fake.thpCalls.size() == 2);
        controls.stop();
        CHECK(fake.thpMode == 0);
        CHECK_FALSE(controls.thpOptOut().active);
        CHECK_FALSE(controls.thpOptOut().error);
    }
    SUBCASE("an older kernel refuses the flag: none keep them") {
        MemoryOps fake;
        fake.thpFlagsError = EINVAL;
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        CHECK(fake.thpCalls == std::vector<Call>{{1, 2}, {1, 0}});
        CHECK(controls.thpOptOut().active);
        CHECK_FALSE(controls.thpOptOut().advisedOnly);
        CHECK_FALSE(controls.thpOptOut().error);
    }
    SUBCASE("a refusal is reported, and the engine runs regardless") {
        MemoryOps fake;
        fake.thpFlagsError = EINVAL;
        fake.thpError = EPERM;
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        CHECK_FALSE(controls.thpOptOut().active);
        REQUIRE(controls.thpOptOut().error);
        CHECK(controls.thpOptOut().error->code == "engine.thp_opt_out");
        CHECK(controls.thpOptOut().error->errnoValue == EPERM);
        controls.stop();
        REQUIRE_FALSE(fake.thpCalls.empty());
        CHECK(fake.thpCalls.back() == Call{1, 0});  // nothing to undo
    }
}

TEST_CASE("memory lock ownership, flags, pending intent and repeated lifecycle") {
    MemoryOps fake;
    {
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        controls.stop();
        CHECK(fake.locks == 0);
        CHECK(fake.unlocks == 0);
        REQUIRE(controls.setMemoryLock(true, false));
        CHECK(controls.memoryLock().requested);
        CHECK_FALSE(controls.memoryLock().active);
        CHECK(fake.locks == 0);
        controls.start();
        CHECK(fake.flags == (MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT));
        CHECK(controls.memoryLock().active);
        REQUIRE(controls.setMemoryLock(true, true));
        CHECK(fake.locks == 1);
        controls.stop();
        controls.stop();
        CHECK(fake.unlocks == 1);
        CHECK(controls.memoryLock().requested);
        controls.start();
        CHECK(fake.locks == 2);
        REQUIRE(controls.setMemoryLock(false, true));
        CHECK_FALSE(controls.memoryLock().active);
        CHECK_FALSE(controls.memoryLock().requested);
        CHECK(fake.unlocks == 2);
        REQUIRE(controls.setMemoryLock(true, true));
    }
    CHECK(fake.unlocks == 3);  // destructor releases the acquired policy
}

TEST_CASE("memory lock failures retain intent and actionable errno, unlock can be retried") {
    MemoryOps fake;
    detail::RuntimeProtection controls(fake.ops);
    for (int value : {ENOMEM, EPERM, EINVAL, EAGAIN}) {
        fake.lockError = value;
        const auto failed = controls.setMemoryLock(true, true);
        REQUIRE_FALSE(failed);
        CHECK(controls.memoryLock().requested);
        CHECK_FALSE(controls.memoryLock().active);
        REQUIRE(controls.memoryLock().error);
        CHECK(controls.memoryLock().error->errnoValue == value);
        CHECK(failed.error().path == "/lock_memory");
        CHECK_FALSE(failed.error().hint.empty());
        if (value == ENOMEM || value == EPERM)
            CHECK(failed.error().hint.find("4096 bytes") != std::string::npos);
        controls.stop();
        CHECK(fake.unlocks == 0);  // failed acquisition must not remove a plug-in's locks
    }
    fake.lockError = 0;
    controls.start();
    REQUIRE(controls.memoryLock().active);
    CHECK_FALSE(controls.memoryLock().error);
    fake.unlockError = EPERM;
    CHECK_FALSE(controls.setMemoryLock(false, true));
    CHECK(controls.memoryLock().active);
    CHECK_FALSE(controls.memoryLock().requested);
    CHECK(controls.memoryLock().error->code == "engine.memory_unlock");
    fake.unlockError = 0;
    controls.stop();
    CHECK_FALSE(controls.memoryLock().active);
    CHECK_FALSE(controls.memoryLock().error);
}

TEST_CASE(
    "EngineApp applies memory policy after start, preserves intent across replacement and stop") {
    MemoryOps fake;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-protection-app-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    {
        AppOptions options;
        options.workdir = dir;
        options.chainDir = dir / "chains";
        options.templateDir = dir / "templates";
        options.projectTemplateDir = dir / "project-templates";
        options.pluginPath = {FIXTURE_CLAP_PATH};
        options.runtimeProtectionOps = fake.ops;
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        auto app = std::move(*created);
        REQUIRE(app->setMemoryLock(true));
        CHECK(app->engineStatus().memoryLock.requested);
        CHECK_FALSE(app->engineStatus().memoryLock.active);
        CHECK(fake.locks == 0);
        CHECK_FALSE(app->startEngine(backend, {.blockSize = 0}));
        CHECK(fake.locks == 0);
        CHECK_FALSE(app->engineStatus().thpOptOut.active);
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK(fake.locks == 1);
        CHECK(app->engineStatus().memoryLock.active);
        CHECK(app->engineStatus().thpOptOut.active);
        REQUIRE(app->transport("stop"));
        CHECK(fake.unlocks == 0);  // transport stop still processes monitored inputs
        REQUIRE(app->newProject({.console9 = false}));
        CHECK(fake.unlocks == 1);
        CHECK(fake.locks == 2);
        app->stopEngine();
        CHECK_FALSE(app->engineStatus().memoryLock.active);
        CHECK_FALSE(app->engineStatus().thpOptOut.active);
        CHECK(app->engineStatus().memoryLock.requested);
        CHECK(fake.unlocks == 2);
        fake.lockError = ENOMEM;
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK(app->engineStatus().running);
        REQUIRE(app->engineStatus().memoryLock.error);
        CHECK(app->engineStatus().memoryLock.error->errnoValue == ENOMEM);
        CHECK_FALSE(app->engineStatus().warnings.empty());
        app->stopEngine();
        CHECK(fake.unlocks == 2);
        CHECK(app->engineStatus().memoryLock.error);
        fake.lockError = 0;
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK_FALSE(app->engineStatus().memoryLock.error);
        // Destructor must stop the backend before releasing the memory policy.
    }
    CHECK(fake.unlocks == 3);
    std::filesystem::remove_all(dir);
}

namespace {
struct LatencyOps {
    std::shared_ptr<RuntimeProtectionOps> ops = std::make_shared<RuntimeProtectionOps>();
    int opens = 0, writes = 0, closes = 0, openError = 0, writeError = 0, closeError = 0;
    bool interruptOpen = false, interruptWrite = false;
    ssize_t writeBytes = sizeof(kCpuIdleLatencyUs);
    LatencyOps() {
        ops->openLatency = [this](const char* path, int flags) {
            ++opens;
            CHECK(std::string(path) == "/dev/cpu_dma_latency");
            CHECK(flags == (O_WRONLY | O_CLOEXEC));
            if (std::exchange(interruptOpen, false)) {
                errno = EINTR;
                return -1;
            }
            errno = openError;
            return openError ? -1 : 73;
        };
        ops->writeLatency = [this](int fd, const void* bytes, std::size_t size) -> ssize_t {
            ++writes;
            CHECK(fd == 73);
            CHECK(size == sizeof(std::int32_t));
            std::int32_t value = -1;
            std::memcpy(&value, bytes, sizeof(value));
            CHECK(value == 20);
            if (std::exchange(interruptWrite, false)) {
                errno = EINTR;
                return -1;
            }
            errno = writeError;
            return writeError ? -1 : writeBytes;
        };
        ops->closeLatency = [this](int fd) {
            ++closes;
            CHECK(fd == 73);
            errno = closeError;
            return closeError ? -1 : 0;
        };
    }
};

struct ProtectionRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("supple-latency-test-" + std::to_string(::getpid()));
    ProtectionRoot() { std::filesystem::remove_all(path); }
    ~ProtectionRoot() { std::filesystem::remove_all(path); }
};

std::unique_ptr<EngineApp> latencyApp(const ProtectionRoot& root, const LatencyOps& fake) {
    AppOptions options;
    options.workdir = root.path;
    options.chainDir = root.path / "chains";
    options.templateDir = root.path / "templates";
    options.projectTemplateDir = root.path / "project-templates";
    options.pluginPath = {FIXTURE_CLAP_PATH};
    options.runtimeProtectionOps = fake.ops;
    auto app = EngineApp::create(options, {.console9 = false});
    REQUIRE(app);
    return std::move(*app);
}
}  // namespace

TEST_CASE("CPU latency owns one CLOEXEC request and retries interrupted acquisition only") {
    LatencyOps fake;
    {
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        controls.stop();
        CHECK(fake.opens == 0);
        REQUIRE(controls.setPreventDeepIdle(true, false));
        CHECK(controls.cpuIdleLatency().requested);
        CHECK_FALSE(controls.cpuIdleLatency().active);
        CHECK(fake.opens == 0);
        fake.interruptOpen = fake.interruptWrite = true;
        controls.start();
        CHECK(fake.opens == 2);
        CHECK(fake.writes == 2);
        CHECK(controls.cpuIdleLatency().active);
        REQUIRE(controls.setPreventDeepIdle(true, true));
        CHECK(fake.opens == 2);
        REQUIRE(controls.setPreventDeepIdle(false, true));
        CHECK(fake.closes == 1);
        controls.stop();
        CHECK(fake.closes == 1);
        REQUIRE(controls.setPreventDeepIdle(true, true));
    }
    CHECK(fake.closes == 2);
}

TEST_CASE(
    "CPU latency failures expose errno, close failed writes and never complete partial writes") {
    LatencyOps fake;
    detail::RuntimeProtection controls(fake.ops);
    for (int value : {EACCES, EPERM, ENOENT, ENODEV, EMFILE}) {
        fake.openError = value;
        auto failed = controls.setPreventDeepIdle(true, true);
        REQUIRE_FALSE(failed);
        CHECK(controls.cpuIdleLatency().requested);
        CHECK_FALSE(controls.cpuIdleLatency().active);
        REQUIRE(controls.cpuIdleLatency().error);
        CHECK(controls.cpuIdleLatency().error->errnoValue == value);
        CHECK(failed.error().path == "/prevent_deep_idle");
        CHECK_FALSE(failed.error().hint.empty());
        if (value == EACCES || value == EPERM)
            CHECK(failed.error().hint.find("group with write access") != std::string::npos);
    }
    CHECK(fake.closes == 0);
    fake.openError = 0;
    fake.writeError = EACCES;
    CHECK_FALSE(controls.setPreventDeepIdle(true, true));
    CHECK(fake.closes == 1);
    CHECK(controls.cpuIdleLatency().error->errnoValue == EACCES);
    fake.writeError = 0;
    for (ssize_t count : {ssize_t{0}, ssize_t{2}}) {
        fake.writeBytes = count;
        const int before = fake.writes;
        CHECK_FALSE(controls.setPreventDeepIdle(true, true));
        CHECK(fake.writes == before + 1);
        CHECK(controls.cpuIdleLatency().error->errnoValue == EIO);
        CHECK_FALSE(controls.cpuIdleLatency().active);
    }
    CHECK(fake.closes == 3);
    fake.writeBytes = sizeof(kCpuIdleLatencyUs);
    REQUIRE(controls.setPreventDeepIdle(true, true));
    CHECK_FALSE(controls.cpuIdleLatency().error);
    fake.closeError = EINTR;
    CHECK_FALSE(controls.setPreventDeepIdle(false, true));
    CHECK_FALSE(controls.cpuIdleLatency().active);
    CHECK(controls.cpuIdleLatency().error->errnoValue == EINTR);
    CHECK(fake.closes == 4);
    controls.stop();
    CHECK(fake.closes == 4);  // close is not retried on Linux
}

TEST_CASE("CPU latency writes the native 20 microsecond value to a temporary file") {
    ProtectionRoot root;
    std::filesystem::create_directories(root.path);
    const auto path = root.path / "latency";
    REQUIRE(writeFileAtomic(path, "xxxx"));
    auto ops = std::make_shared<RuntimeProtectionOps>();
    int fd = -1;
    // Only this injectable open is replaced; production write/close use a temp file.
    ops->openLatency = [&](const char*, int flags) { return fd = ::open(path.c_str(), flags); };
    detail::RuntimeProtection controls(ops);
    REQUIRE(controls.setPreventDeepIdle(true, true));
    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    const auto bytes = readFile(path);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == sizeof(std::int32_t));
    std::int32_t value = -1;
    std::memcpy(&value, bytes->data(), sizeof(value));
    CHECK(value == kCpuIdleLatencyUs);
    controls.stop();
    CHECK(::fcntl(fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
}

TEST_CASE("CPU latency follows EngineApp startup, project replacement, stop and destruction") {
    ProtectionRoot root;
    LatencyOps fake;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    {
        auto app = latencyApp(root, fake);
        REQUIRE(app->setPreventDeepIdle(true));
        CHECK(app->engineStatus().cpuIdleLatency.requested);
        CHECK_FALSE(app->startEngine(backend, {.blockSize = 0}));
        CHECK(fake.opens == 0);
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK(app->engineStatus().cpuIdleLatency.active);
        REQUIRE(app->transport("stop"));
        CHECK(fake.closes == 0);
        REQUIRE(app->newProject({.console9 = false}));
        CHECK(fake.closes == 1);
        CHECK(fake.opens == 2);
        app->stopEngine();
        CHECK(fake.closes == 2);
        CHECK(app->engineStatus().cpuIdleLatency.requested);
        CHECK_FALSE(app->engineStatus().cpuIdleLatency.active);
        fake.openError = EACCES;
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK(app->engineStatus().running);
        CHECK_FALSE(app->engineStatus().cpuIdleLatency.active);
        CHECK(app->engineStatus().cpuIdleLatency.error->errnoValue == EACCES);
        CHECK_FALSE(app->engineStatus().warnings.empty());
        app->stopEngine();
        CHECK(fake.closes == 2);
        fake.openError = 0;
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK_FALSE(app->engineStatus().cpuIdleLatency.error);
    }
    CHECK(fake.closes == 3);
}

TEST_CASE("CPU latency releases on device recovery and failed initial load") {
    ProtectionRoot root;
    LatencyOps fake;
    FakeJack& jack = fakeJack();
    jack.reset(1024);
    JackBackend backend({.library = FakeJack::kPath});
    auto app = latencyApp(root, fake);
    app->setFixedClock(1'000'000);
    REQUIRE(app->setPreventDeepIdle(true));
    REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
    jack.setRate(44100);
    app->tick();
    CHECK_FALSE(app->engine());
    CHECK(fake.closes == 1);
    app->tick();
    REQUIRE(app->engine());
    CHECK(fake.opens == 2);
    app->stopEngine();
    CHECK(fake.closes == 2);
    jack.reset();
    auto bad = app->head();
    bad.version = app->versions().nextVersion();
    model::Route route;
    route.from.track = model::Id{model::Kind::Track, 0};
    route.to.track = route.from.track;
    bad.routes = bad.routes.with({model::Kind::Route, 1}, bad.version,
                                 std::make_shared<const model::Route>(route));
    REQUIRE(app->versions().commit(std::move(bad), {}, {}, {}));
    CHECK_FALSE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
    CHECK(fake.opens == 2);  // no request acquired until initial plan loads
    CHECK_FALSE(app->engine());
    CHECK_FALSE(app->engineStatus().cpuIdleLatency.active);
}

TEST_CASE("engine status serializes both optional protection failures without losing warnings") {
    ProtectionRoot root;
    LatencyOps fake;
    fake.openError = EACCES;
    fake.ops->lockMemory = [](int) {
        errno = ENOMEM;
        return -1;
    };
    fake.ops->unlockMemory = [] {
        FAIL("no memory policy was acquired");
        return 0;
    };
    fake.ops->memoryLimit = [](rlimit* limit) {
        limit->rlim_cur = 4096;
        return 0;
    };
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    auto app = latencyApp(root, fake);
    REQUIRE(app->setMemoryLock(true));
    REQUIRE(app->setPreventDeepIdle(true));
    REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto result = bus.execute(
        {"engine.status", json::Value::object(), {cmd::Origin::Type::Test, "protection-test", {}}});
    REQUIRE(result);
    CHECK((*result)["memory_lock"]["error"]["errno"].asInt() == ENOMEM);
    CHECK((*result)["cpu_idle_latency"]["error"]["errno"].asInt() == EACCES);
    CHECK((*result)["cpu_idle_latency"]["requested"].asBool());
    CHECK_FALSE((*result)["cpu_idle_latency"]["active"].asBool());
    CHECK((*result)["warnings"].size() >= 2);
    app->stopEngine();
    CHECK(app->engineStatus().memoryLock.error);
    CHECK(app->engineStatus().cpuIdleLatency.error);
}
