// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A scratch folder for one test, unique to this run: two runs of the suite (two presets, a
// person and an agent) sharing a TMPDIR must not delete each other's files mid-test.
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

namespace tempdir {

inline std::filesystem::path uniquePath(const std::string& stem) {
    static std::random_device device;
    static std::uint64_t counter = 0;
    const auto now =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t value = (std::uint64_t{device()} << 32 | device()) ^ now ^ ++counter;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string suffix;
    for (int shift = 60; shift >= 0; shift -= 4) suffix += kHex[(value >> shift) & 15];
    return std::filesystem::temp_directory_path() / (stem + "-" + suffix);
}

// Created empty, removed with its contents when it goes out of scope.
struct Dir {
    std::filesystem::path path;
    explicit Dir(const std::string& stem) : path(uniquePath(stem)) {
        std::filesystem::create_directories(path);
    }
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    ~Dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace tempdir
