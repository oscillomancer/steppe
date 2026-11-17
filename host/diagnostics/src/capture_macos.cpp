// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include "capsule.h"
#include "trace.h"
// macOS in-process capture (ADR-0083, topic 4). The failing process reads its own
// threads and images through Mach calls on its own task: no task_for_pid, no entitlement,
// no other process touched. It runs in the terminal handler, so it allocates nothing on
// the heap, takes no lock and formats nothing: the capsule and every scratch buffer are
// static, memory is read with mach_vm_read_overwrite (which fails instead of faulting)
// and dyld's image list is read as plain memory, never through dyld calls that lock.
// task_threads hands back its list in kernel-mapped memory, which is returned with
// vm_deallocate; that is the only allocation, and it is not the heap.
//
// The handler runs once. Each other thread is suspended only while its registers and
// stack are read and is resumed before the next, on every path.
namespace supple::diagnostics::detail {
namespace {
// Of the worker's one second: thread reads end at 400 ms, listing the images and finding
// the module of each frame at 450 ms, sending at 500 ms, and the wait for the supervisor's
// acknowledgement gets what is left. The supervisor reads for kCollectionBudget (600 ms)
// from when it receives the terminal packet, which the worker published after it took its
// own clock (the handler passes that reading in), so the supervisor's deadline always falls
// after the worker's and the send must end with a margin before it; a late capsule is lost and the
// fault thread is not_collected.
constexpr std::uint64_t kThreadBudget = 400'000'000ULL, kImageBudget = 450'000'000ULL,
                        kSendBudget = 500'000'000ULL, kHold = 1'000'000'000ULL;
static_assert(kThreadBudget < kImageBudget && kImageBudget < kSendBudget &&
              kSendBudget + 50'000'000ULL <= kCollectionBudget && kCollectionBudget < kHold);
constexpr std::size_t kMaxImages = 4096, kMaxCommands = 32 * 1024;
#if defined(__aarch64__)
constexpr bool kArm64 = true;
#else
constexpr bool kArm64 = false;
#endif

CapsuleData capsule;  // zero-filled static storage
mach_port_t mainThread = MACH_PORT_NULL;
std::uint64_t imageBases[kMaxImages];
std::uint32_t imageCount = 0;
alignas(8) unsigned char commandBytes[kMaxCommands];
// What an image's header said, kept so it is read once: its __TEXT range (size 0 if it had
// none or could not be read) and its UUID.
struct ImageText {
    std::uint64_t vmaddr, size;
    std::uint8_t uuid[16];
    bool examined, hasUuid;
};
ImageText images[kMaxImages];

bool readOwn(void*, std::uint64_t address, void* out, std::size_t bytes) {
    mach_vm_size_t got = 0;
    const kern_return_t result = mach_vm_read_overwrite(
        mach_task_self(), static_cast<mach_vm_address_t>(address),
        static_cast<mach_vm_size_t>(bytes),
        static_cast<mach_vm_address_t>(reinterpret_cast<std::uintptr_t>(out)), &got);
    return result == KERN_SUCCESS && got == bytes;
}
const StackReader kReader{&readOwn, nullptr};

// The images dyld lists, as load addresses only; headers are read when a frame needs one.
// Stops at `deadline`: the images after it are simply not known, and their frames carry no
// identity.
void listImages(std::uint64_t deadline) {
    task_dyld_info_data_t info{};
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
            KERN_SUCCESS ||
        info.all_image_info_addr == 0)
        return;
    dyld_all_image_infos all{};
    const std::size_t want = std::min<std::size_t>(sizeof(all), info.all_image_info_size);
    if (want < offsetof(dyld_all_image_infos, infoArray) + sizeof(all.infoArray) ||
        !readOwn(nullptr, info.all_image_info_addr, &all, want))
        return;
    const std::uint32_t total = all.infoArrayCount;
    const auto array = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(all.infoArray));
    if (array == 0) return;
    dyld_image_info chunk[64];
    for (std::uint32_t done = 0; done < total && imageCount < kMaxImages;) {
        if (monotonicNanoseconds() >= deadline) break;
        const std::uint32_t n = std::min<std::uint32_t>(64, total - done);
        if (!readOwn(nullptr, array + std::uint64_t{done} * sizeof(dyld_image_info), chunk,
                     n * sizeof(dyld_image_info)))
            break;
        for (std::uint32_t i = 0; i < n && imageCount < kMaxImages; ++i) {
            const auto base = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(chunk[i].imageLoadAddress));
            if (base != 0) imageBases[imageCount++] = base;
        }
        done += n;
    }
}

bool inModules(std::uint64_t address) {
    for (std::uint32_t i = 0; i < capsule.header.moduleCount; ++i)
        if (address >= capsule.modules[i].start && address < capsule.modules[i].end) return true;
    return false;
}

// Adds the image whose text holds `address`, if dyld's list has it: the listed image with
// the highest load address at or below it, whose header is read and bounded. An image's
// header is read once, whatever the outcome: what it said is kept, so a later frame in
// the same image costs a comparison, and a damaged stack of addresses that fall in no
// image's text cannot make the handler read the same headers again and again.
void addImageFor(std::uint64_t address) {
    if (inModules(address)) return;
    std::uint64_t best = 0;
    std::uint32_t bestIndex = 0;
    for (std::uint32_t i = 0; i < imageCount; ++i)
        if (imageBases[i] <= address && imageBases[i] > best) {
            best = imageBases[i];
            bestIndex = i;
        }
    if (best == 0) return;
    ImageText& known = images[bestIndex];
    if (!known.examined) {
        known.examined = true;
        mach_header_64 header{};
        if (!readOwn(nullptr, best, &header, sizeof(header)) || header.magic != MH_MAGIC_64 ||
            header.sizeofcmds == 0 || header.sizeofcmds > kMaxCommands || header.ncmds > 1024 ||
            !readOwn(nullptr, best + sizeof(header), commandBytes, header.sizeofcmds))
            return;
        std::size_t at = 0;
        for (std::uint32_t i = 0; i < header.ncmds; ++i) {
            if (header.sizeofcmds - at < sizeof(load_command)) break;
            load_command command{};
            std::memcpy(&command, commandBytes + at, sizeof(command));
            if (command.cmdsize < sizeof(load_command) || command.cmdsize > header.sizeofcmds - at)
                break;
            if (command.cmd == LC_SEGMENT_64 && command.cmdsize >= sizeof(segment_command_64)) {
                segment_command_64 segment{};
                std::memcpy(&segment, commandBytes + at, sizeof(segment));
                if (std::strncmp(segment.segname, "__TEXT", sizeof(segment.segname)) == 0) {
                    known.vmaddr = segment.vmaddr;
                    known.size = segment.vmsize;
                }
            } else if (command.cmd == LC_UUID && command.cmdsize >= sizeof(uuid_command)) {
                uuid_command found{};
                std::memcpy(&found, commandBytes + at, sizeof(found));
                std::memcpy(known.uuid, found.uuid, sizeof(known.uuid));
                known.hasUuid = true;
            }
            at += command.cmdsize;
        }
    }
    if (known.size == 0 || address - best >= known.size) return;
    if (capsule.header.moduleCount >= kCapsuleModules) {
        capsule.header.flags |= kCapsuleModulesTruncated;
        return;
    }
    CapsuleModule& module = capsule.modules[capsule.header.moduleCount++];
    module.start = best;
    module.end = best + known.size;
    module.bias = best - known.vmaddr;
    if (known.hasUuid) {
        std::memcpy(module.uuid, known.uuid, sizeof(known.uuid));
        module.hasUuid = 1;
    }
}

Missing missingFor(kern_return_t result, bool suspending) {
    if (result == KERN_TERMINATED || result == KERN_INVALID_ARGUMENT) return Missing::ThreadExited;
    return suspending ? Missing::AccessDenied : Missing::RegistersUnreadable;
}

// Registers of a suspended thread. Only arm64 and x86-64 are known.
kern_return_t registersOf(thread_act_t thread, CapsuleRegs& regs) {
#if defined(__aarch64__)
    arm_thread_state64_t state{};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    const kern_return_t result = thread_get_state(thread, ARM_THREAD_STATE64,
                                                  reinterpret_cast<thread_state_t>(&state), &count);
    if (result == KERN_SUCCESS) {
        regs.pc = arm_thread_state64_get_pc(state);
        regs.fp = arm_thread_state64_get_fp(state);
        regs.sp = arm_thread_state64_get_sp(state);
    }
    return result;
#elif defined(__x86_64__)
    x86_thread_state64_t state{};
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    const kern_return_t result = thread_get_state(thread, x86_THREAD_STATE64,
                                                  reinterpret_cast<thread_state_t>(&state), &count);
    if (result == KERN_SUCCESS) {
        regs.pc = state.__rip;
        regs.fp = state.__rbp;
        regs.sp = state.__rsp;
    }
    return result;
#else
    (void)thread;
    (void)regs;
    return KERN_NOT_SUPPORTED;
#endif
}

// `id` is the thread's Mach port name, the key its registration in the execution table
// uses; only atomic loads of that table, so nothing here allocates or locks.
CapsuleThreadData* newThread(Role role, std::uint64_t id) {
    if (capsule.header.threadCount >= kCapsuleThreads) {
        ++capsule.header.dropped;
        capsule.header.flags |= kCapsuleThreadsDropped;
        return nullptr;
    }
    CapsuleThreadData& thread = capsule.threads[capsule.header.threadCount++];
    thread.head.role = static_cast<std::uint8_t>(role);
    thread.head.missing = static_cast<std::uint8_t>(Missing::None);
    const diagctx::SlotInfo slot = diagctx::findThread(id);
    thread.head.registered = static_cast<std::uint8_t>(slot.role);
    thread.head.worker = slot.ordinal;
    return &thread;
}
void unavailable(CapsuleThreadData& thread, Missing why) {
    thread.head.missing = static_cast<std::uint8_t>(why);
    thread.head.frameCount = 0;
}

// The calling thread, from the registers the handler was given, or from its own frame
// when it has none (terminate, abort): it is parked in the handler, so its stack is
// stable and readable without suspending it.
void captureSelf(const Packet& terminal, std::uint64_t deadline) {
    CapsuleThreadData* thread = newThread(Role::Fault, diagctx::currentThreadId());
    if (!thread) return;
    CapsuleRegs regs{terminal.pc, terminal.sp, terminal.frame};
    if (regs.sp == 0) {  // no registers at all (a null pc alone is a real crash site)
        const auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
        regs.pc = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        regs.sp = frame;
        regs.fp = frame != 0 ? *reinterpret_cast<const std::uintptr_t*>(frame) : 0;
        if (kArm64) regs.pc &= 0x0000ffffffffffffULL;
    }
    walkStack(kReader, regs, kArm64, *thread, deadline);
}

void captureOthers(std::uint64_t deadline) {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS || !list) return;
    const mach_port_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        const thread_act_t port = list[i];
        if (port != self) {
            CapsuleThreadData* thread =
                newThread(port == mainThread ? Role::Main : Role::Other, port);
            if (thread) {
                if (monotonicNanoseconds() >= deadline) {
                    unavailable(*thread, Missing::Deadline);
                } else if (const kern_return_t stopped = thread_suspend(port);
                           stopped != KERN_SUCCESS) {
                    unavailable(*thread, missingFor(stopped, true));
                } else {
                    CapsuleRegs regs;
                    const kern_return_t got = registersOf(port, regs);
                    if (got == KERN_SUCCESS)
                        walkStack(kReader, regs, kArm64, *thread, deadline);
                    else
                        unavailable(*thread, got == KERN_NOT_SUPPORTED ? Missing::Unsupported
                                                                       : missingFor(got, false));
                    (void)thread_resume(port);  // on every path once suspended
                }
            }
        }
        (void)mach_port_deallocate(mach_task_self(), port);
    }
    (void)mach_port_deallocate(mach_task_self(), self);
    (void)vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list),
                        count * sizeof(thread_act_t));
}

// Modules for every frame address found, added as they are met, until `deadline`; frames
// met after it carry no identity, which the parser accepts.
void resolveModules(std::uint64_t deadline) {
    for (std::uint32_t t = 0; t < capsule.header.threadCount; ++t) {
        const CapsuleThreadData& thread = capsule.threads[t];
        for (std::uint32_t f = 0; f < thread.head.frameCount; ++f) {
            if (monotonicNanoseconds() >= deadline) return;
            addImageFor(thread.frames[f]);
        }
    }
}
}  // namespace

void rememberMainThread() noexcept { mainThread = mach_thread_self(); }

void captureAndHold(Native& endpoint, const Packet& terminal, std::uint64_t start) noexcept {
    capsule.header.magic = kCapsuleMagic;
    captureSelf(terminal, start + kThreadBudget);
    captureOthers(start + kThreadBudget);
    listImages(start + kImageBudget);
    resolveModules(start + kImageBudget);
    const int fd = static_cast<int>(endpoint.channel);
    // If the capsule cannot be sent in time the supervisor stops waiting at its own
    // deadline; the terminal packet it already has is what matters most.
    (void)sendCapsule(fd, terminal, &capsule, start + kSendBudget);
    const std::uint64_t spent = monotonicNanoseconds() - start;
    if (spent < kHold) awaitAcknowledgementFor(endpoint, kHold - spent);
}
}  // namespace supple::diagnostics::detail
