// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Bridge exit/handle-transfer adapter backed by the shared child-process helper.
#pragma once
#include <supple/base/result.h>
#include <supple/host/child_process.h>
#include <filesystem>
#include "bridge_platform.h"
namespace supple::host {
struct BridgeTestAccess;  // defined by the tests alone
}
namespace supple::host::bridge {
class NativeChild {
  public:
    static Result<std::unique_ptr<NativeChild>> launch(const std::filesystem::path& exe,
                                                       Resource control);
    ~NativeChild();
    int pid() const { return pid_; }
    Waitable waitable() const;
    bool alive() const;
    void kill() const;
    int exitStatus() const;
#if defined(_WIN32)
    Resource duplicate(Resource resource) const;
    void revoke(Resource resource) const;
#endif
  private:
    friend struct ::supple::host::BridgeTestAccess;
    NativeChild() = default;
    int pid_ = -1;
    std::unique_ptr<ChildProcess> owner_;
#if defined(_WIN32)
    HANDLE process_ = nullptr;  // borrowed from owner_
#else
    int death_ = -1;
#endif
};
std::filesystem::path ownExecutable();
}  // namespace supple::host::bridge
