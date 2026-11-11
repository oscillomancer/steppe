// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <avrt.h>
#include <mmsystem.h>

#include <algorithm>
#include "midi_output.h"
namespace supple::engine::detail {
namespace {
class WinMidiSink final : public MidiSink {
    struct LongBuffer {
        MIDIHDR header{};
        std::array<char, 10> bytes{};
        std::atomic<bool> ready{true};
        bool prepared = false;
    };
    struct Port {
        HMIDIOUT handle = nullptr;
        std::array<LongBuffer, 8> longBuffers;
    };
    std::array<Port, kMidiOutputPorts> ports_;
    std::array<MIDIOUTCAPSW, kMidiOutputPorts> topology_{};
    std::array<MMRESULT, kMidiOutputPorts> capsErrors_{};
    unsigned deviceCount_ = 0;
    double frequency_ = 1;
    HANDLE timer_ = nullptr;
    HANDLE mmcss_ = nullptr;  // sender thread only
    static void CALLBACK completion(HMIDIOUT, UINT message, DWORD_PTR, DWORD_PTR data, DWORD_PTR) {
        if (message == MOM_DONE) {
            auto* header = reinterpret_cast<MIDIHDR*>(data);
            auto* buffer = reinterpret_cast<LongBuffer*>(header->dwUser);
            buffer->ready.store(true, std::memory_order_release);
        }
    }
    static void close(Port& port) {
        if (!port.handle) return;
        // Reset returns all long buffers before their storage can be released.
        midiOutReset(port.handle);
        for (auto& buffer : port.longBuffers) {
            if (buffer.prepared)
                midiOutUnprepareHeader(port.handle, &buffer.header, sizeof(MIDIHDR));
            buffer.prepared = false;
        }
        midiOutClose(port.handle);
        port.handle = nullptr;
    }

  public:
    ~WinMidiSink() override { stop(); }
    Status start() override {
        stop();
        names.clear();
        warnings.clear();
        latencySeconds.clear();
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        frequency_ = static_cast<double>(frequency.QuadPart);
        // Windows 10 1803+. This is a process-owned timer, no global timer change.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        if (!timer_) {
            warnings.push_back({"MIDI outputs", GetLastError(),
                                "could not create high-resolution MIDI timer",
                                "use Windows 10 version 1803 or newer; release unneeded handles "
                                "and restart the engine"});
            return okStatus();
        }
        deviceCount_ = midiOutGetNumDevs();
        for (unsigned i = 0; i < std::min<unsigned>(deviceCount_, kMidiOutputPorts); ++i) {
            auto& caps = topology_[i];
            caps = {};
            auto error = midiOutGetDevCapsW(i, &caps, sizeof(caps));
            capsErrors_[i] = error;
            const int size =
                WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
            std::string name(static_cast<std::size_t>(size), '\0');
            if (size)
                WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name.data(), size, nullptr,
                                    nullptr);
            if (!name.empty()) name.pop_back();
            if (name.empty()) name = "MIDI output " + std::to_string(i + 1);
            auto& port = ports_[names.size()];
            if (!error)
                error = midiOutOpen(&port.handle, i, reinterpret_cast<DWORD_PTR>(&completion), 0,
                                    CALLBACK_FUNCTION);
            for (auto& buffer : port.longBuffers) {
                if (error) break;
                buffer.ready.store(true);
                buffer.header = {};
                buffer.header.lpData = buffer.bytes.data();
                buffer.header.dwBufferLength = static_cast<DWORD>(buffer.bytes.size());
                buffer.header.dwUser = reinterpret_cast<DWORD_PTR>(&buffer);
                error = midiOutPrepareHeader(port.handle, &buffer.header, sizeof(MIDIHDR));
                if (!error) buffer.prepared = true;
            }
            if (error) {
                close(port);
                warnings.push_back({std::move(name), error, "WinMM MIDI output setup failed"});
            } else {
                names.push_back(std::move(name));
                latencySeconds.push_back(0);  // WinMM exposes no measured output latency
            }
        }
        return okStatus();
    }
    void stop() override {
        for (auto& port : ports_) close(port);
        if (timer_) CloseHandle(timer_);
        timer_ = nullptr;
    }
    double now() const SUPPLE_RT override {
        LARGE_INTEGER ticks;
        QueryPerformanceCounter(&ticks);
        return static_cast<double>(ticks.QuadPart) / frequency_;
    }
    void prepareThread() override {
        DWORD task = 0;
        mmcss_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (mmcss_)
            AvSetMmThreadPriority(mmcss_, AVRT_PRIORITY_HIGH);
        else
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    void releaseThread() override {
        if (mmcss_) AvRevertMmThreadCharacteristics(mmcss_);
        mmcss_ = nullptr;
    }
    void waitUntil(double seconds) override {
        if (!timer_) {
            MidiSink::waitUntil(seconds);
            return;
        }
        const double delay = std::clamp(seconds - now(), 0.0, .001);
        if (delay <= 0) return;
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(delay * 1e7));
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_, INFINITE);
        else
            MidiSink::waitUntil(seconds);
    }
    bool send(const TimedMidiOutput& timed) override {
        const auto& e = timed.event;
        auto& port = ports_[e.port];
        if (e.bytes[0] != 0xf0) {
            DWORD message = e.bytes[0];
            for (unsigned b = 1; b < e.size; ++b) message |= DWORD(e.bytes[b]) << (8 * b);
            return midiOutShortMsg(port.handle, message) == MMSYSERR_NOERROR;
        }
        for (auto& buffer : port.longBuffers) {
            if (!buffer.ready.exchange(false, std::memory_order_acq_rel)) continue;
            std::copy_n(e.bytes, e.size, buffer.bytes.data());
            buffer.header.dwBufferLength = e.size;
            const auto error = midiOutLongMsg(port.handle, &buffer.header, sizeof(MIDIHDR));
            if (error) buffer.ready.store(true, std::memory_order_release);
            return error == MMSYSERR_NOERROR;
        }
        return false;  // all eight SysEx buffers in flight; bounded loss
    }
    bool changed() override {
        if (midiOutGetNumDevs() != deviceCount_) return true;
        for (unsigned i = 0; i < std::min<unsigned>(deviceCount_, kMidiOutputPorts); ++i) {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != capsErrors_[i] ||
                caps.wMid != topology_[i].wMid || caps.wPid != topology_[i].wPid ||
                std::wstring_view(caps.szPname) != topology_[i].szPname)
                return true;
        }
        return false;
    }
};
}  // namespace
std::unique_ptr<MidiSink> nativeMidiSink(MidiSource*) { return std::make_unique<WinMidiSink>(); }
}  // namespace supple::engine::detail
