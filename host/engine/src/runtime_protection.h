// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <memory>

#include "supple/base/result.h"
#include "supple/engine/runtime_protection.h"

namespace supple::engine::detail {

// Owned by one EngineApp in the host process; never used on an audio path.
class RuntimeProtection {
  public:
    explicit RuntimeProtection(std::shared_ptr<RuntimeProtectionOps> ops);
    ~RuntimeProtection();
    RuntimeProtection(const RuntimeProtection&) = delete;
    RuntimeProtection& operator=(const RuntimeProtection&) = delete;
    Status setMemoryLock(bool requested, bool running);
    Status setPreventDeepIdle(bool requested, bool running);
    const RuntimeProtectionStatus& cpuIdleLatency() const { return latency_; }
    const ThpOptOutStatus& thpOptOut() const { return thp_; }
    void start();
    void stop();
    const RuntimeProtectionStatus& memoryLock() const { return memory_; }

  private:
    Status applyMemory();
    Status releaseMemory();
    Status applyLatency();
    Status releaseLatency();
    void applyThp();
    void releaseThp();
    std::shared_ptr<RuntimeProtectionOps> ops_;
    RuntimeProtectionStatus memory_;
    RuntimeProtectionStatus latency_;
    ThpOptOutStatus thp_;
    int latencyFd_ = -1;
};

}  // namespace supple::engine::detail
