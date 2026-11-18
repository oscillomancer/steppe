// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <cstring>
#include "self_capture_internal.h"
// macOS: a bridge captures itself as the worker does (capture_macos.cpp reads every thread of
// the own task with Mach calls on its own task, and the images dyld lists), then writes the
// result into the slot instead of sending it. The modules the bridge recorded while cold are
// added to the ones the frames resolved.
namespace supple::diagnostics::selfcapture {
bool identifyImage(const void* address, BridgeModule& out) {
    Dl_info info{};
    if (!::dladdr(address, &info) || !info.dli_fbase) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
    if (header->magic != MH_MAGIC_64 || header->sizeofcmds == 0 || header->ncmds > 1024 ||
        header->sizeofcmds > 1024 * 1024)
        return false;
    out = BridgeModule{};
    bool text = false;
    const auto* at = reinterpret_cast<const unsigned char*>(header + 1);
    std::uint32_t left = header->sizeofcmds;
    for (std::uint32_t i = 0; i < header->ncmds; ++i) {
        if (left < sizeof(load_command)) break;
        load_command command;
        std::memcpy(&command, at, sizeof(command));
        if (command.cmdsize < sizeof(load_command) || command.cmdsize > left) break;
        if (command.cmd == LC_SEGMENT_64 && command.cmdsize >= sizeof(segment_command_64)) {
            segment_command_64 segment;
            std::memcpy(&segment, at, sizeof(segment));
            if (std::strncmp(segment.segname, "__TEXT", sizeof(segment.segname)) == 0) {
                out.start = base;
                out.end = base + segment.vmsize;
                out.bias = base - segment.vmaddr;
                text = true;
            }
        } else if (command.cmd == LC_UUID && command.cmdsize >= sizeof(uuid_command)) {
            uuid_command found;
            std::memcpy(&found, at, sizeof(found));
            std::memcpy(out.id, found.uuid, sizeof(found.uuid));
            out.idLength = sizeof(found.uuid);
        }
        at += command.cmdsize;
        left -= command.cmdsize;
    }
    return text && address >= reinterpret_cast<const void*>(out.start) &&
           address < reinterpret_cast<const void*>(out.end);
}

bool captureAndPublishMac(const Fault& fault, std::uint64_t start) noexcept {
    detail::Packet registers;
    registers.pc = fault.pc;
    registers.sp = fault.sp;
    registers.frame = fault.fp;
    detail::CapsuleData& own = detail::captureOwnProcess(registers, start);
    static BridgeModule modules[kMaxModules];
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < own.header.moduleCount && count < kMaxModules; ++i) {
        BridgeModule& m = modules[count++];
        m = BridgeModule{};
        m.bias = own.modules[i].bias;
        m.start = own.modules[i].start;
        m.end = own.modules[i].end;
        if (own.modules[i].hasUuid) {
            std::memcpy(m.id, own.modules[i].uuid, sizeof(own.modules[i].uuid));
            m.idLength = sizeof(own.modules[i].uuid);
        }
    }
    static BridgeModule recorded[kMaxModules];
    std::uint32_t flags = 0;
    const std::uint32_t known = copyRecorded(recorded, kMaxModules, flags);
    for (std::uint32_t i = 0; i < known; ++i) {
        bool present = false;
        for (std::uint32_t j = 0; j < count; ++j)
            if (modules[j].start == recorded[i].start) present = true;
        if (present) continue;
        if (count >= kMaxModules) {
            flags |= detail::kCapsuleModulesTruncated;
            break;
        }
        modules[count++] = recorded[i];
    }
    detail::CapsuleHeader header = own.header;
    header.reserved = static_cast<std::uint32_t>(fault.tid);
    header.flags |= flags;
    return publishCapsule(header, modules, count, own.threads, own.header.threadCount, start);
}
}  // namespace supple::diagnostics::selfcapture
