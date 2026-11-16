// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "runtime_protection.h"

#include <fcntl.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif
#include <utility>

#include <cerrno>
#include <cstring>

namespace supple::engine {

#if defined(__linux__)
RuntimeProtectionOps::RuntimeProtectionOps()
    : lockMemory([](int flags) { return ::mlockall(flags); }),
      unlockMemory([] { return ::munlockall(); }),
      memoryLimit([](rlimit* limit) { return ::getrlimit(RLIMIT_MEMLOCK, limit); }),
      openLatency([](const char* path, int flags) { return ::open(path, flags); }),
      writeLatency(
          [](int fd, const void* bytes, std::size_t size) { return ::write(fd, bytes, size); }),
      closeLatency([](int fd) { return ::close(fd); }),
      thpDisable([](unsigned long disable, unsigned long flags) {
          return ::prctl(PR_SET_THP_DISABLE, disable, flags, 0UL, 0UL);
      }) {}
#else
RuntimeProtectionOps::RuntimeProtectionOps()
    : lockMemory([](int) {
          errno = ENOTSUP;
          return -1;
      }),
      unlockMemory([] { return 0; }),
      memoryLimit([](MemoryLimit*) {
          errno = ENOTSUP;
          return -1;
      }),
      openLatency([](const char*, int) {
          errno = ENOTSUP;
          return -1;
      }),
      writeLatency([](int, const void*, std::size_t) -> ProtectionWriteSize {
          errno = ENOTSUP;
          return -1;
      }),
      closeLatency([](int) { return 0; }),
      thpDisable([](unsigned long, unsigned long) {
          errno = ENOTSUP;
          return -1;
      }) {}
#endif

namespace detail {
namespace {
Status failure(RuntimeProtectionStatus& state, int value, const char* code, const char* operation,
               const char* path, std::string hint) {
    RuntimeProtectionError error{code,
                                 std::string(operation) + ": " + std::strerror(value) + " (errno " +
                                     std::to_string(value) + ")",
                                 path, std::move(hint), value};
    state.error = error;
    return Error(error.code, error.message, error.path, error.hint);
}
}  // namespace

RuntimeProtection::RuntimeProtection(std::shared_ptr<RuntimeProtectionOps> ops)
    : ops_(ops ? std::move(ops) : std::make_shared<RuntimeProtectionOps>()) {}

RuntimeProtection::~RuntimeProtection() { stop(); }

Status RuntimeProtection::setMemoryLock(bool requested, bool running) {
    memory_.requested = requested;
    if (!requested) {
        if (memory_.active) return releaseMemory();
        memory_.error.reset();
        return {};
    }
    return running ? applyMemory() : Status{};
}

Status RuntimeProtection::applyMemory() {
    if (memory_.active) return {};
#if defined(__linux__)
    constexpr int flags = MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT;
#else
    constexpr int flags = 0;
#endif
    if (ops_->lockMemory(flags) == 0) {
        memory_.active = true;
        memory_.error.reset();
        return {};
    }
    const int value = errno;
    std::string hint = "disable memory locking, or retry after resolving the OS error";
    if (value == ENOMEM || value == EPERM) {
        MemoryLimit limit{};
        const std::string allowance = ops_->memoryLimit(&limit) != 0 ? "unavailable"
                                      : limit.rlim_cur == kUnlimitedMemory
                                          ? "unlimited"
                                          : std::to_string(limit.rlim_cur) + " bytes";
        hint =
            "disable memory locking or reduce mapped memory; ask an administrator to review "
            "the account's memlock allowance (currently " +
            allowance + ")";
    } else if (value == EINVAL) {
        hint = "disable memory locking; the kernel does not support the requested flags";
    }
    if (value == ENOTSUP)
        hint =
            "process-wide memory locking with future/on-fault semantics is Linux-only; disable the "
            "option";
    return failure(memory_, value, "engine.memory_lock", "mlockall", "/lock_memory",
                   std::move(hint));
}

Status RuntimeProtection::releaseMemory() {
    if (!memory_.active) return {};
    if (ops_->unlockMemory() != 0) {
        const int value = errno;
        return failure(memory_, value, "engine.memory_unlock", "munlockall", "/lock_memory",
                       "retry disabling memory locking; process exit also releases all locks");
    }
    memory_.active = false;
    memory_.error.reset();
    return {};
}

Status RuntimeProtection::setPreventDeepIdle(bool requested, bool running) {
    latency_.requested = requested;
    if (!requested) {
        if (latencyFd_ >= 0) return releaseLatency();
        latency_.error.reset();
        return {};
    }
    return running ? applyLatency() : Status{};
}

Status RuntimeProtection::applyLatency() {
    if (latencyFd_ >= 0) return {};
    int fd;
    do {
#if defined(__linux__)
        fd = ops_->openLatency("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
#else
        fd = ops_->openLatency("/dev/cpu_dma_latency", 0);
#endif
    } while (fd < 0 && errno == EINTR);
    int value = 0;
    const char* operation = "open /dev/cpu_dma_latency";
    if (fd < 0) {
        value = errno;
    } else {
        ProtectionWriteSize written;
        do {
            written = ops_->writeLatency(fd, &kCpuIdleLatencyUs, sizeof(kCpuIdleLatencyUs));
        } while (written < 0 && errno == EINTR);
        if (written == static_cast<ProtectionWriteSize>(sizeof(kCpuIdleLatencyUs))) {
            latencyFd_ = fd;
            latency_.active = true;
            latency_.error.reset();
            return {};
        }
        value = written < 0 ? errno : EIO;
        operation = "write /dev/cpu_dma_latency";
        // A short device write is invalid; never send a second partial integer.
        // Linux closes even on EINTR, so retrying could close a reused fd.
        (void)ops_->closeLatency(fd);
    }
    std::string hint = "disable the CPU idle option, or retry after resolving the device error";
    if (value == EACCES || value == EPERM)
        hint =
            "the account needs membership in a group with write access to the device "
            "(commonly audio); ask an administrator or disable the option";
    else if (value == ENOENT || value == ENODEV)
        hint = "the CPU latency device is unavailable; disable the option on this system";
    if (value == ENOTSUP)
        hint = "the process CPU idle-latency request is Linux-only; disable the option";
    return failure(latency_, value, "engine.cpu_idle_latency", operation, "/prevent_deep_idle",
                   std::move(hint));
}

Status RuntimeProtection::releaseLatency() {
    if (latencyFd_ < 0) return {};
    const int fd = std::exchange(latencyFd_, -1);
    latency_.active = false;
    if (ops_->closeLatency(fd) != 0) {
        const int value = errno;
        return failure(latency_, value, "engine.cpu_idle_latency", "close /dev/cpu_dma_latency",
                       "/prevent_deep_idle",
                       "the descriptor is released; inspect the OS close error before retrying");
    }
    latency_.error.reset();
    return {};
}

void RuntimeProtection::applyThp() {
#if defined(__linux__)
    // Linux 6.18's PR_THP_DISABLE_EXCEPT_ADVISED, for older headers.
    constexpr unsigned long kThpExceptAdvised = 1UL << 1;
    if (thp_.active) return;
    // Advised regions keep huge pages where the kernel can say so; an older
    // kernel refuses the flag (EINVAL), and then no region does.
    bool advisedOnly = true;
    int result = ops_->thpDisable(1, kThpExceptAdvised);
    if (result != 0 && errno == EINVAL) {
        advisedOnly = false;
        result = ops_->thpDisable(1, 0);
    }
    if (result == 0) {
        thp_.active = true;
        thp_.advisedOnly = advisedOnly;
        thp_.error.reset();
        return;
    }
    const int value = errno;
    thp_.error = RuntimeProtectionError{
        "engine.thp_opt_out",
        std::string("prctl(PR_SET_THP_DISABLE): ") + std::strerror(value) + " (errno " +
            std::to_string(value) + ")",
        "/thp_opt_out",
        "the kernel refused to keep the process out of transparent huge pages; audio threads "
        "may stall while khugepaged collapses memory (set transparent_hugepage to madvise or "
        "never to avoid it)",
        value};
#endif
}

void RuntimeProtection::releaseThp() {
    if (!thp_.active) return;
    (void)ops_->thpDisable(0, 0);
    thp_.active = false;
    thp_.advisedOnly = false;
}

void RuntimeProtection::start() {
    applyThp();
    if (memory_.requested) (void)applyMemory();
    if (latency_.requested) (void)applyLatency();
}

void RuntimeProtection::stop() {
    (void)releaseLatency();
    (void)releaseMemory();
    releaseThp();
}

}  // namespace detail
}  // namespace supple::engine
