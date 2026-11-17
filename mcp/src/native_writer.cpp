// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "native_writer.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
namespace supple::mcp::detail {
namespace {
Error outputError() {
    return Error("mcp.write", "stdio output could not be delivered", "/stdout",
                 "check the output consumer; an edit may have run, so inspect before retrying");
}
}  // namespace
struct NativeWriter::Impl {
    WakeEvent progress, cancel, work;
    std::atomic<bool> occupied{false}, failed{false}, stopping{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> published{false};
    std::string inbox;
    bool ready = false;
    std::thread thread;
#if defined(_WIN32)
    HANDLE output = INVALID_HANDLE_VALUE;
#else
    int output = -1, originalFlags = -1;
    int originalNoSigpipe = -1;  // macOS: the descriptor's own SIGPIPE opt-out before setup
#endif
    ~Impl() {
        stopping.store(true);
        cancel.notify();
        if (thread.joinable()) {
#if defined(_WIN32)
            // Cancellation can precede entry into synchronous WriteFile. Retry
            // only during teardown and retain the handle/buffer through join.
            while (WaitForSingleObject(thread.native_handle(), 10) == WAIT_TIMEOUT)
                CancelSynchronousIo(thread.native_handle());
#endif
            thread.join();
        }
#if defined(_WIN32)
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
#else
        if (output >= 0) {
            // dup shares status flags with the owner's exclusive transport fd.
            const int flags = ::fcntl(output, F_GETFL);
            if (flags >= 0 && originalFlags >= 0)
                (void)::fcntl(output, F_SETFL,
                              (flags & ~O_NONBLOCK) | (originalFlags & O_NONBLOCK));
#if defined(F_SETNOSIGPIPE)
            if (originalNoSigpipe == 0) (void)::fcntl(output, F_SETNOSIGPIPE, 0);
#endif
            ::close(output);
        }
#endif
    }
    bool writePacket(const std::string& packet) {
        std::size_t sent = 0;
        while (sent < packet.size() && !stopping.load()) {
#if defined(_WIN32)
            DWORD count = 0;
            if (!WriteFile(output, packet.data() + sent,
                           static_cast<DWORD>(std::min<std::size_t>(packet.size() - sent, 65536)),
                           &count, nullptr) ||
                !count)
                return false;
            sent += count;
#else
            const auto count = ::write(output, packet.data() + sent, packet.size() - sent);
            if (count > 0) {
                sent += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                WaitItem sources[]{{output, kWaitWrite}, {cancel.waitable(), kWaitRead}};
                const int readyCount = waitFor(sources, -1);
                if (readyCount < 0 && errno == EINTR) continue;
                if (readyCount < 0 || sources[1].ready ||
                    (sources[0].ready & (kWaitHangup | kWaitError | kWaitInvalid)))
                    return false;
                continue;
            }
            return false;
#endif
        }
        return sent == packet.size();
    }
    void run() {
#if !defined(_WIN32)
        // Do not change process/caller disposition; retain the block until this
        // owned thread exits, including any pending SIGPIPE from an EPIPE.
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) failed.store(true);
#endif
        {
            std::lock_guard lock(mutex);
            ready = true;
        }
        cv.notify_one();
        if (failed.load()) {
            progress.notify();
            return;
        }
        for (;;) {
            bool written = false;
            {
                if (!published.exchange(false, std::memory_order_acquire)) {
                    if (stopping.load()) return;
                    WaitItem sources[]{{work.waitable(), kWaitRead},
                                       {cancel.waitable(), kWaitRead}};
                    const int count = waitFor(sources, -1);
                    if (count < 0 && errno == EINTR) continue;
                    if (count < 0) {
                        failed.store(true);
                        progress.notify();
                        return;
                    }
                    work.drain();
                    if (stopping.load()) return;
                    continue;
                }
                std::string packet = std::move(inbox);
                written = writePacket(packet);
            }  // free packet storage before releasing its reserved capacity
            if (!written) failed.store(true);
            occupied.store(false, std::memory_order_release);
            progress.notify();
            if (!written) return;
        }
    }
};
#if defined(_WIN32)
std::intptr_t checkedNativeHandle(int fd) {
    if (fd < 0) return -1;
    const auto prior = _set_thread_local_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {});
    const auto handle = _get_osfhandle(fd);
    _set_thread_local_invalid_parameter_handler(prior);
    return handle;
}
#endif
NativeWriter::NativeWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeWriter::~NativeWriter() = default;
Result<std::unique_ptr<NativeWriter>> NativeWriter::create(int fd) {
    if (fd < 0) return outputError();
#if defined(_WIN32)
    const auto borrowed = reinterpret_cast<HANDLE>(checkedNativeHandle(fd));
    if (borrowed == INVALID_HANDLE_VALUE) return outputError();
#else
    const int flagsBeforeSetup = ::fcntl(fd, F_GETFL);
    if (flagsBeforeSetup < 0 || (flagsBeforeSetup & O_ACCMODE) == O_RDONLY) return outputError();
#endif
    // Validate before creating wake descriptors, which might reuse a closed fd.
    auto impl = std::make_unique<Impl>();
    if (!impl->progress.waitable().valid() || !impl->cancel.waitable().valid() ||
        !impl->work.waitable().valid())
        return outputError();
#if defined(_WIN32)
    if (!DuplicateHandle(GetCurrentProcess(), borrowed, GetCurrentProcess(), &impl->output, 0,
                         FALSE, DUPLICATE_SAME_ACCESS))
        return outputError();
#else
    impl->output = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (impl->output < 0) return outputError();
    impl->originalFlags = ::fcntl(impl->output, F_GETFL);
    if (impl->originalFlags < 0 || (impl->originalFlags & O_ACCMODE) == O_RDONLY ||
        ::fcntl(impl->output, F_SETFL, impl->originalFlags | O_NONBLOCK) != 0)
        return outputError();
#if defined(F_SETNOSIGPIPE)
    // macOS raises SIGPIPE for the process, not for the writing thread, so the
    // writer thread's signal mask does not hold it there. Opt this descriptor
    // out for the writer's lifetime; like the nonblocking bit, the setting is
    // shared with the owner's descriptor and restored after join.
    impl->originalNoSigpipe = ::fcntl(impl->output, F_GETNOSIGPIPE);
    if (impl->originalNoSigpipe < 0 || ::fcntl(impl->output, F_SETNOSIGPIPE, 1) != 0) {
        impl->originalNoSigpipe = -1;
        return outputError();
    }
#endif
#endif
    auto* state = impl.get();
    impl->thread = std::thread([state] { state->run(); });
    {
        std::unique_lock lock(impl->mutex);
        impl->cv.wait(lock, [&] { return impl->ready; });
    }
    if (impl->failed.load()) return outputError();
    return std::unique_ptr<NativeWriter>(new NativeWriter(std::move(impl)));
}
bool NativeWriter::idle() const { return !impl_->occupied.load(std::memory_order_acquire); }
Status NativeWriter::status() const {
    return impl_->failed.load() ? Status(outputError()) : Status{};
}
Status NativeWriter::submit(std::string packet) {
    if (packet.size() > kCapacity)
        return Error("mcp.response_too_large", "response packet exceeds 64 MiB", "/result",
                     "narrow the query; check whether an edit already ran");
    SUPPLE_TRY(status());
    if (impl_->occupied.exchange(true))
        return Error("mcp.write", "response capacity was not reserved", "/stdout",
                     "reserve before dispatch");
    if (impl_->failed.load()) {
        impl_->occupied.store(false);
        return outputError();
    }
    impl_->inbox = std::move(packet);
    impl_->published.store(true, std::memory_order_release);
    impl_->work.notify();
    return {};
}
Waitable NativeWriter::progress() const { return impl_->progress.waitable(); }
void NativeWriter::drainProgress() { impl_->progress.drain(); }
}  // namespace supple::mcp::detail
