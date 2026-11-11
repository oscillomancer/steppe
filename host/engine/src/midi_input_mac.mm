// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <algorithm>
#include "midi_input.h"
#include "supple/product.h"
namespace supple::engine::detail {
namespace {
class CoreMidiSource final : public MidiSource {
    MIDIClientRef client_ = 0;
    MIDIPortRef input_ = 0;
    MidiInputBuffer* buffer_ = nullptr;
    std::array<MIDIEndpointRef, kMidiInputPorts> sources_{};
    double secondsPerTick_ = 0;
    ItemCount sourceCount_ = 0;
    // SysEx7 being assembled per port (ADR-0071): only an MTC full frame
    // (eight data bytes between F0 and F7) is kept; longer messages are drops.
    struct SysEx {
        std::array<std::uint8_t, 8> data{};
        std::size_t size = 0;
        bool overflow = false;
    };
    std::array<SysEx, kMidiInputPorts> sysex_{};
    void sysex7(std::size_t port, double stamp, UInt32 first, UInt32 second) SUPPLE_RT {
        if (port >= sysex_.size()) {
            buffer_->drop();
            return;
        }
        auto& s = sysex_[port];
        const auto status = (first >> 20) & 0xf;  // 0 complete, 1 start, 2 continue, 3 end
        const auto count = std::min<UInt32>((first >> 16) & 0xf, 6);
        if (status == 0 || status == 1) {
            s.size = 0;
            s.overflow = false;
        }
        const std::uint8_t bytes[6] = {
            static_cast<std::uint8_t>(first >> 8),   static_cast<std::uint8_t>(first),
            static_cast<std::uint8_t>(second >> 24), static_cast<std::uint8_t>(second >> 16),
            static_cast<std::uint8_t>(second >> 8),  static_cast<std::uint8_t>(second)};
        for (UInt32 b = 0; b < count; ++b) {
            if (s.size == s.data.size()) {
                s.overflow = true;
                break;
            }
            s.data[s.size++] = bytes[b] & 0x7f;
        }
        if (status != 0 && status != 3) return;
        if (s.overflow || s.size != s.data.size()) {
            buffer_->drop();
        } else {
            std::array<std::uint8_t, 10> message{};
            message[0] = 0xF0;
            std::copy(s.data.begin(), s.data.end(), message.begin() + 1);
            message[9] = 0xF7;
            buffer_->pushSync(port, stamp, message.data(), message.size());
        }
        s.size = 0;
        s.overflow = false;
    }
    void receive(const MIDIEventList* list, std::size_t port) SUPPLE_RT {
        const auto* packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; ++p) {
            const double stamp = packet->timeStamp
                                     ? static_cast<double>(packet->timeStamp) * secondsPerTick_
                                     : now();
            for (UInt32 i = 0; i < packet->wordCount;) {
                const auto word = packet->words[i];
                const auto type = word >> 28;
                if (type == 2 || type == 1) {
                    // MIDI 1.0 channel voice, or system real-time/common (one
                    // word): push sends clock, start/continue/stop, song
                    // position and MTC quarter frames to the sync messages and
                    // counts the other system messages as drops (ADR-0071).
                    const auto message =
                        ((word >> 16) & 0xff) | ((word >> 8) & 0xff) << 8 | (word & 0xff) << 16;
                    buffer_->push(port, stamp, message);
                } else if (type == 3 && i + 1 < packet->wordCount) {
                    sysex7(port, stamp, word, packet->words[i + 1]);  // SysEx7, two words
                } else {
                    buffer_->drop();  // MIDI 2, utility/flex, data 128
                }
                // UMP type determines its size, including unsupported messages.
                constexpr unsigned words[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
                i += words[type];
            }
            packet = MIDIEventPacketNext(packet);
        }
    }

  public:
    ~CoreMidiSource() override { stop(); }
    Status start(MidiInputBuffer& buffer) override {
        stop();
        buffer_ = &buffer;
        mach_timebase_info_data_t timebase{};
        mach_timebase_info(&timebase);
        secondsPerTick_ = static_cast<double>(timebase.numer) / timebase.denom * 1e-9;
        const auto title =
            CFStringCreateWithCString(nullptr, product::kName.data(), kCFStringEncodingUTF8);
        auto error = MIDIClientCreate(title, nullptr, nullptr, &client_);
        if (!error)
            error = MIDIInputPortCreateWithProtocol(
                client_, title, kMIDIProtocol_1_0, &input_,
                ^(const MIDIEventList* list, void* context) {
                  receive(list, reinterpret_cast<std::uintptr_t>(context));
                });
        CFRelease(title);
        sourceCount_ = MIDIGetNumberOfSources();
        const auto count = std::min<ItemCount>(sourceCount_, kMidiInputPorts);
        const auto setupError = error;
        if (setupError && !count)
            buffer.warnings.push_back({"MIDI inputs", setupError, "CoreMIDI input setup failed"});
        for (ItemCount i = 0; i < count; ++i) {
            sources_[i] = MIDIGetSource(i);
            CFStringRef name = nullptr;
            MIDIObjectGetStringProperty(sources_[i], kMIDIPropertyDisplayName, &name);
            std::string text;
            if (name) {
                const auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(name),
                                                                        kCFStringEncodingUTF8) +
                                      1;
                text.resize(static_cast<std::size_t>(capacity));
                if (CFStringGetCString(name, text.data(), capacity, kCFStringEncodingUTF8))
                    text.resize(std::char_traits<char>::length(text.c_str()));
                else
                    text.clear();
                CFRelease(name);
            }
            if (text.empty()) text = "MIDI input " + std::to_string(i + 1);
            error = setupError
                        ? setupError
                        : MIDIPortConnectSource(input_, sources_[i],
                                                reinterpret_cast<void*>(buffer.names.size()));
            if (error)
                buffer.warnings.push_back({std::move(text), error, "CoreMIDI input setup failed"});
            else
                buffer.names.push_back(std::move(text));
        }
        return okStatus();
    }
    void stop() override {
        if (input_) MIDIPortDispose(input_);
        input_ = 0;
        if (client_) MIDIClientDispose(client_);
        client_ = 0;
        buffer_ = nullptr;
    }
    double now() const SUPPLE_RT override {
        return static_cast<double>(mach_absolute_time()) * secondsPerTick_;
    }
    bool changed() override {
        if (MIDIGetNumberOfSources() != sourceCount_) return true;
        for (std::size_t i = 0; i < std::min<ItemCount>(sourceCount_, kMidiInputPorts); ++i)
            if (MIDIGetSource(i) != sources_[i]) return true;
        return false;
    }
};
}  // namespace
std::unique_ptr<MidiSource> nativeMidiSource() { return std::make_unique<CoreMidiSource>(); }
}  // namespace supple::engine::detail
