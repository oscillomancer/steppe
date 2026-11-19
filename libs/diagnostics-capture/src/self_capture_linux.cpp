// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>
#include "self_capture_internal.h"
#include "supple/diagcap/trace.h"
// Linux: how a bridge identifies its images while cold, and how its handler walks the fault
// thread. The reader is process_vm_readv on the own process: a system call that fails with
// EFAULT instead of faulting, so a damaged stack ends the walk and never the handler.
namespace supple::diagnostics::selfcapture {
namespace {
#if defined(__aarch64__)
constexpr bool kArm64 = true;
#else
constexpr bool kArm64 = false;
#endif

bool readOwn(void*, std::uint64_t address, void* out, std::size_t bytes) {
    iovec local{out, bytes};
    iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), bytes};
    return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
}
std::optional<std::string> readMaps() {
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string text;
    char buffer[16384];
    for (;;) {
        const auto n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            return n == 0 ? std::optional<std::string>(std::move(text)) : std::nullopt;
        }
        text.append(buffer, static_cast<std::size_t>(n));
        if (text.size() > 8 * 1024 * 1024) {
            ::close(fd);
            return std::nullopt;
        }
    }
}
}  // namespace

bool identifyImage(const void* address, BridgeModule& out) {
    const auto where = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address));
    const auto text = readMaps();
    if (!text) return false;
    const auto maps = detail::parseMaps(*text);
    const detail::Mapping* holder = nullptr;
    for (const auto& m : maps)
        if (m.named && m.inode != 0 && where >= m.start && where < m.end) holder = &m;
    if (!holder) return false;
    // The image: every mapping of the same file. Its base is the lowest with offset 0, its
    // text the span of the executable ones.
    std::uint64_t base = ~std::uint64_t{0}, low = ~std::uint64_t{0}, high = 0;
    for (const auto& m : maps) {
        if (m.inode != holder->inode || m.device != holder->device) continue;
        if (m.offset == 0 && m.start < base) base = m.start;
        if (m.execute) {
            if (m.start < low) low = m.start;
            if (m.end > high) high = m.end;
        }
    }
    if (base == ~std::uint64_t{0} || low >= high || where < low || where >= high) return false;
    const detail::ReadMemory reader = [](std::uint64_t a, void* buffer, std::size_t n) {
        return readOwn(nullptr, a, buffer, n);
    };
    std::uint64_t bias = 0;
    const auto id = detail::elfBuildId(reader, base, &bias);
    out = BridgeModule{};
    out.start = low;
    out.end = high;
    out.bias = bias;
    if (id && id->size() % 2 == 0 && id->size() / 2 <= kModuleIdBytes) {
        const auto nibble = [](char c) -> std::uint8_t {
            return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        for (std::size_t i = 0; i < id->size(); i += 2)
            out.id[i / 2] = static_cast<std::uint8_t>(nibble((*id)[i]) << 4 | nibble((*id)[i + 1]));
        out.idLength = static_cast<std::uint32_t>(id->size() / 2);
    }
    return true;
}

void walkFault(const Fault& fault, detail::CapsuleThreadData& thread,
               std::uint64_t deadline) noexcept {
    const detail::StackReader reader{&readOwn, nullptr};
    const detail::CapsuleRegs regs{fault.pc, fault.sp, fault.fp};
    detail::walkStack(reader, regs, kArm64, thread, deadline);
}
}  // namespace supple::diagnostics::selfcapture
