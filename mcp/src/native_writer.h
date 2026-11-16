// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <supple/base/result.h>
#include <supple/base/waitable.h>
#include <memory>
#include <string>
namespace supple::mcp::detail {
#if defined(_WIN32)
std::intptr_t checkedNativeHandle(int fd);
#endif
// One complete packet reserves the capacity until its bytes/storage are released.
// This conservative reservation bounds queued + in-flight output without knowing
// a command's reply size before executing it. Publication has no producer lock
// or output-I/O wait; the worker alone writes bytes.
class NativeWriter {
  public:
    static constexpr std::size_t kCapacity = 64 * 1024 * 1024;
    static Result<std::unique_ptr<NativeWriter>> create(int fd);
    ~NativeWriter();
    bool idle() const;
    Status status() const;
    Status submit(std::string packet);
    Waitable progress() const;
    void drainProgress();

  private:
    struct Impl;
    explicit NativeWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}  // namespace supple::mcp::detail
