// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A device that starts and never calls back, as a stalled driver: callbacks
// would run on a thread of its own, so the engine waits for them with a
// bound. stop() makes the final frames == 0 call on the caller, as the
// Backend contract asks.
#pragma once

#include <supple/engine/realtime.h>

#include <array>
#include <string>
#include <utility>

namespace supple::engine::test {

class StalledBackend : public Backend {
  public:
    Status start(const BackendConfig& config, Callback callback) override {
        config_ = config;
        callback_ = std::move(callback);
        return okStatus();
    }
    void stop() override {
        if (!callback_) return;
        std::array<float, 1> in{}, out{};
        std::array<const float*, 1> ins{in.data()};
        std::array<float*, 1> outs{out.data()};
        callback_(ins.data(), outs.data(), 0);
        callback_ = nullptr;
    }
    BackendConfig config() const override { return config_; }
    std::string name() const override { return "stalled"; }
    bool ownsThread() const override { return true; }
    Status prepareLiveThread(std::thread::native_handle_type) override { return okStatus(); }

  private:
    BackendConfig config_;
    Callback callback_;
};

}  // namespace supple::engine::test
