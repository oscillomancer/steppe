// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "supple/base/result.h"
namespace supple::diagnostics::detail {
enum class Message : std::uint32_t { Boot = 1, Ready = 2, Expected = 3, Terminal = 4 };
struct Packet {
    std::uint32_t magic = 0x43525348, version = 1;
    Message kind = Message::Boot;
    std::uint32_t role = 0, code = 0, cause = 0;
    std::uint64_t pid = 0, pc = 0, frame = 0;
    std::array<char, 33> incident{};
};
static_assert(sizeof(Packet) <= 128);
struct Native {
    std::intptr_t channel = -1, process = -1, event = -1, ack = -1;
    void* shared = nullptr;
    std::uint32_t seen = 0;
    bool trusted = true;
    // POSIX caches waitpid observation independently of channel ownership.
    bool exited = false, statusKnown = false;
    int status = 0;
};
Result<Native> launch(int argc, char** argv, const Packet& boot);
Result<Native> adopt(std::string_view value, Packet& boot);
bool receive(Native& native, Packet& packet);  // collector, parked until work/exit
bool publish(Native& native, const Packet& packet) noexcept;
void acknowledge(Native& native) noexcept;
void awaitAcknowledgement(Native& native) noexcept;  // terminal only, <=1 second
inline bool windowsFatalStatus(std::uint32_t code, std::optional<std::uint32_t> expected) {
    // A Main return can use every DWORD value, including exception-shaped
    // values. The caller withholds expected when a terminal capsule exists.
    return (!expected || *expected != code) && (code & 0xc0000000U) == 0xc0000000U;
}
int wait(Native& native, bool& fatal, std::optional<std::uint32_t> expected = {});
void close(Native& native);
void closeProtocolStdio();
std::uint64_t pid();
Result<std::string> incidentId();
bool valid(const Packet& packet);
}  // namespace supple::diagnostics::detail
