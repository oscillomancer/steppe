// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A gate between a test and the gain fixture in a bridge: the test learns
// that the plug-in is inside a given call, and holds it there, with no clock.
//
// SUPPLE_TEST_LIFECYCLE_GATE names a file of at least 4096 bytes, read when a
// gain is made. Its first words: 0 the plug-in entered the gated call, 1 the
// test lets it go on, 2 it went on, 3 which call is gated (GatedCall). The
// gated call sets word 0, spins until word 1 is set (no system call, so a
// sanitizer sees a hang, not a call), then sets word 2. Both sides map the
// file with mapGateFile; the test side is LifecycleGate.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace supple::test {

inline constexpr const char* kLifecycleGateVariable = "SUPPLE_TEST_LIFECYCLE_GATE";
inline constexpr unsigned kGateBytes = 4096;

enum class GatedCall : unsigned {
    StartProcessing = 0,  // the default of a zeroed file
    Reset = 1,
    Flush = 2,  // params flush
    StopProcessing = 3,
    Process = 4,  // the first process call
    Activate = 5,
};

// Maps an existing gate file, shared; nullptr when it cannot. Never unmapped
// by the plug-in side (it lives as long as the bridge).
inline std::atomic<unsigned>* mapGateFile(const char* path) {
    if (!path) return nullptr;
#if defined(_WIN32)
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0, kGateBytes, nullptr);
    CloseHandle(file);
    if (!mapping) return nullptr;
    void* words = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kGateBytes);
    CloseHandle(mapping);  // the view keeps the mapping
    return static_cast<std::atomic<unsigned>*>(words);
#else
    const int fd = ::open(path, O_RDWR);
    if (fd < 0) return nullptr;
    void* words = ::mmap(nullptr, kGateBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    return words == MAP_FAILED ? nullptr : static_cast<std::atomic<unsigned>*>(words);
#endif
}

// The plug-in side: holds `call` at the gate when it is the gated one.
inline void passGate(std::atomic<unsigned>* gate, GatedCall call) {
    if (!gate || gate[3].load(std::memory_order_acquire) != static_cast<unsigned>(call)) return;
    gate[0].store(1, std::memory_order_release);
    while (!gate[1].load(std::memory_order_acquire)) {
    }
    gate[2].store(1, std::memory_order_release);
}

// The test side: a fresh gate file for `call`, named in the environment
// while it lives, so bridges spawned meanwhile map it. A bridge keeps the
// mapping it made; the destructor lets any still-held call go on.
class LifecycleGate {
  public:
    explicit LifecycleGate(GatedCall call) {
        static std::atomic<unsigned> serial{0};
        path_ = std::filesystem::temp_directory_path() /
                ("supple-lifecycle-gate-" + std::to_string(processId()) + "-" +
                 std::to_string(serial.fetch_add(1)));
        {
            std::ofstream zeros(path_, std::ios::binary | std::ios::trunc);
            const std::string bytes(kGateBytes, '\0');
            zeros.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        words_ = mapGateFile(path_.string().c_str());
        if (!words_) return;
        words_[3].store(static_cast<unsigned>(call), std::memory_order_release);
        setVariable(path_.string().c_str());
    }
    ~LifecycleGate() {
        setVariable(nullptr);
        if (words_) {
            words_[1].store(1, std::memory_order_release);
#if defined(_WIN32)
            UnmapViewOfFile(words_);
#else
            ::munmap(words_, kGateBytes);
#endif
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    LifecycleGate(const LifecycleGate&) = delete;
    LifecycleGate& operator=(const LifecycleGate&) = delete;
    bool mapped() const { return words_ != nullptr; }
    // Takes the file out of the environment: bridges spawned from now on
    // are not gated; those spawned before keep their mapping.
    void hide() const { setVariable(nullptr); }
    // Waits until the plug-in is inside the gated call.
    bool entered() const { return reached(0); }
    // Lets the held call go on; wentOn() waits until it has.
    void release() const {
        if (words_) words_[1].store(1, std::memory_order_release);
    }
    bool wentOn() const { return reached(2); }
    // Whether the held call has gone on, now (never, when its bridge died).
    bool passed() const { return words_ && words_[2].load(std::memory_order_acquire) != 0; }

  private:
    // The 60 s bound only guards against a bridge that died: the gate, not
    // the clock, orders the test.
    bool reached(int word) const {
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (words_ && !words_[word].load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < giveUp)
            std::this_thread::yield();
        return words_ && words_[word].load(std::memory_order_acquire) != 0;
    }
    static long processId() {
#if defined(_WIN32)
        return static_cast<long>(GetCurrentProcessId());
#else
        return static_cast<long>(::getpid());
#endif
    }
    static void setVariable(const char* value) {
#if defined(_WIN32)
        _putenv_s(kLifecycleGateVariable, value ? value : "");
#else
        if (value)
            ::setenv(kLifecycleGateVariable, value, 1);
        else
            ::unsetenv(kLifecycleGateVariable);
#endif
    }
    std::filesystem::path path_;
    std::atomic<unsigned>* words_ = nullptr;
};

}  // namespace supple::test
