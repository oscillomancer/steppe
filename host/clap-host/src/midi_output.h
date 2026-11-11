// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: plug-in note and MIDI output as OutputEvents (ADR-0068), shared by
// the CLAP and VST3 backings. Every function here runs on the audio thread:
// no allocation, no locks.
#pragma once

#include <cmath>
#include <cstdint>
#include <span>

#include "supple/base/rt.h"
#include "supple/host/clap_host.h"

namespace supple::host::midiout {

// The data bytes a MIDI 1.0 short message with this status byte carries, or
// -1 when it starts no short message: a data byte, SysEx (F0/F7) or an
// undefined status (F4, F5, F9, FD).
inline int dataBytes(std::uint8_t status) SUPPLE_RT {
    if (status < 0x80) return -1;
    if (status < 0xf0) {
        const auto type = status & 0xf0;
        return type == 0xc0 || type == 0xd0 ? 1 : 2;
    }
    switch (status) {
        case 0xf1:
        case 0xf3:
            return 1;
        case 0xf2:
            return 2;
        case 0xf6:
        case 0xf8:
        case 0xfa:
        case 0xfb:
        case 0xfc:
        case 0xfe:
        case 0xff:
            return 0;
        default:
            return -1;
    }
}

// A Midi event in canonical form (unused data bytes zero); false when the
// message is malformed (a data byte with its top bit set) or not a short one.
inline bool shortMessage(std::uint8_t status, std::uint8_t d1, std::uint8_t d2,
                         OutputEvent& out) SUPPLE_RT {
    const int n = dataBytes(status);
    if (n < 0 || (n > 0 && d1 > 0x7f) || (n > 1 && d2 > 0x7f)) return false;
    out.kind = OutputEvent::Kind::Midi;
    out.midi[0] = status;
    out.midi[1] = n > 0 ? d1 : 0;
    out.midi[2] = n > 1 ? d2 : 0;
    return true;
}

// A Universal MIDI Packet as one MIDI 1.0 short message: message type 1
// (system) and 2 (MIDI 1.0 channel voice) exactly; type 4 (MIDI 2.0 channel
// voice) by the MIDI 2.0 down-conversion: note on/off (velocity 16 to 7 bits,
// a note on never at 0), poly and channel pressure, CC (32 to 7 bits), pitch
// bend (32 to 14 bits) and a program change without bank. Everything else
// (per-note controllers and pitch bend, RPN/NRPN, a program change with bank,
// SysEx and data messages) has no single short message: false.
inline bool fromUmp(const std::uint32_t data[4], OutputEvent& out) SUPPLE_RT {
    const std::uint32_t w0 = data[0], w1 = data[1];
    const auto type = w0 >> 28;
    const auto b1 = static_cast<std::uint8_t>(w0 >> 16), b2 = static_cast<std::uint8_t>(w0 >> 8),
               b3 = static_cast<std::uint8_t>(w0);
    if (type == 0x1) return b1 >= 0xf0 && shortMessage(b1, b2, b3, out);
    if (type == 0x2) return b1 >= 0x80 && b1 < 0xf0 && shortMessage(b1, b2, b3, out);
    if (type != 0x4) return false;
    const std::uint8_t opcode = b1 & 0xf0, channel = b1 & 0x0f;
    const auto top7 = static_cast<std::uint8_t>(w1 >> 25);
    switch (opcode) {
        case 0x80:
        case 0x90: {
            auto velocity = static_cast<std::uint8_t>(w1 >> 25);  // the top 7 of 16 bits
            if (opcode == 0x90 && velocity == 0) velocity = 1;
            return shortMessage(opcode | channel, b2 & 0x7f, velocity, out);
        }
        case 0xa0:
        case 0xb0:
            return shortMessage(opcode | channel, b2 & 0x7f, top7, out);
        case 0xc0:
            if (b3 & 1) return false;  // bank valid: a bank select and a program change
            return shortMessage(0xc0 | channel, static_cast<std::uint8_t>((w1 >> 24) & 0x7f), 0,
                                out);
        case 0xd0:
            return shortMessage(0xd0 | channel, top7, 0, out);
        case 0xe0: {
            const auto bend = w1 >> 18;  // 14 bits
            return shortMessage(0xe0 | channel, static_cast<std::uint8_t>(bend & 0x7f),
                                static_cast<std::uint8_t>(bend >> 7), out);
        }
        default:
            return false;
    }
}

// A note event; false when its address is malformed (an on needs a channel
// 0..15 and a key 0..127; an off or a choke may use -1 wildcards) or its
// velocity is not finite. The velocity is clamped to 0..1, a choke's is 0.
inline bool note(OutputEvent::Kind kind, std::int32_t port, std::int32_t channel, std::int32_t key,
                 std::int32_t noteId, double velocity, OutputEvent& out) SUPPLE_RT {
    const int lowest = kind == OutputEvent::Kind::NoteOn ? 0 : -1;
    if (port < lowest || port > 0x7fff || channel < lowest || channel > 15 || key < lowest ||
        key > 127 || noteId < -1 || !std::isfinite(velocity))
        return false;
    out.kind = kind;
    out.port = static_cast<std::int16_t>(port);
    out.channel = static_cast<std::int16_t>(channel);
    out.key = static_cast<std::int16_t>(key);
    out.noteId = noteId;
    out.value =
        kind == OutputEvent::Kind::NoteChoke ? 0.0 : std::fmin(std::fmax(velocity, 0.0), 1.0);
    return true;
}

// A 0..1 amount as a 7-bit MIDI value (rounded, clamped); false when not finite.
inline bool seven(double amount, std::uint8_t& out) SUPPLE_RT {
    if (!std::isfinite(amount)) return false;
    out = static_cast<std::uint8_t>(std::lround(std::fmin(std::fmax(amount, 0.0), 1.0) * 127.0));
    return true;
}

// The output time within a block of `frames` (0..frames-1).
inline std::uint32_t blockTime(std::int64_t time, std::uint32_t frames) SUPPLE_RT {
    if (time <= 0 || frames == 0) return 0;
    return time >= frames ? frames - 1 : static_cast<std::uint32_t>(time);
}

// Stable in-place sort by time: insertion sort, allocation-free, linear when
// the plug-in already emitted its events in order (the usual case).
inline void sortByTime(std::span<OutputEvent> events) SUPPLE_RT {
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i - 1].time <= events[i].time) continue;
        const OutputEvent moving = events[i];
        std::size_t j = i;
        for (; j > 0 && events[j - 1].time > moving.time; --j) events[j] = events[j - 1];
        events[j] = moving;
    }
}

}  // namespace supple::host::midiout
