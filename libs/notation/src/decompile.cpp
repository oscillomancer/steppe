// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/notation/notation.h>

#include "internal.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace supple::notation {
namespace {

struct Timeline {
    Timeline(int ticks, const DecompileOptions& opts) : ppq(ticks), options(opts) {}
    int ppq;
    const DecompileOptions& options;
    mutable std::vector<midi::Meter> meters;
    std::vector<std::int64_t> starts{0};
    midi::Meter meter(int bar) const {
        while (meters.size() < static_cast<std::size_t>(bar)) {
            const int next = static_cast<int>(meters.size()) + 1;
            meters.push_back(options.meterAt ? options.meterAt(next) : midi::Meter{});
        }
        return meters[static_cast<std::size_t>(bar - 1)];
    }
    std::int64_t start(int bar) const {
        return starts[static_cast<std::size_t>(std::clamp(bar, 1, static_cast<int>(starts.size())) -
                                               1)];
    }
    int bar(std::int64_t tick) const {
        return std::max(1, static_cast<int>(std::upper_bound(starts.begin(), starts.end(), tick) -
                                            starts.begin()));
    }
    std::string position(std::int64_t tick) const {
        const int b = bar(tick);
        const auto m = meter(b);
        const auto beatTicks = std::int64_t(ppq) * 4 / m.denominator;
        const auto within = tick - start(b);
        const bool wholeBeat = (std::int64_t(ppq) * 4) % m.denominator == 0;
        const auto beat = wholeBeat ? within / beatTicks + 1 : 1;
        const auto offset = wholeBeat ? within % beatTicks : within;
        std::string result = std::to_string(b) + "." + std::to_string(beat);
        if (offset != 0) result += "+" + std::to_string(offset) + "t";
        return result;
    }
    std::string length(std::int64_t tick) const {
        int bars = bar(tick) - 1;
        const auto offset = tick - start(bars + 1);
        std::string result = std::to_string(bars);
        if (offset != 0) result += "+" + std::to_string(offset) + "t";
        return result;
    }
};

struct Group {
    std::int64_t start = 0, length = 0;
    int velocity = 96, off = 0;
    std::vector<int> pitches;
};
struct Voice {
    std::vector<Group> groups;
    std::int64_t cursor = 0;
};
struct Piece {
    std::int64_t tick = 0;
    std::int64_t length = 0;
    std::string token;
};

std::string noteToken(const Group& group, std::int64_t length, int ppq, int baseVelocity, bool tied,
                      const std::optional<harmony::Key>& key) {
    std::string result;
    if (group.pitches.size() == 1)
        result = internal::pitchName(group.pitches[0], key);
    else {
        result = "[";
        for (std::size_t i = 0; i < group.pitches.size(); ++i) {
            if (i != 0) result += " ";
            result += internal::pitchName(group.pitches[i], key);
        }
        result += "]";
    }
    result += ":" + internal::durationName(length, ppq);
    if (group.velocity != baseVelocity || group.off != 0) {
        result += ":" + std::to_string(group.velocity);
        if (group.off != 0) result += "/" + std::to_string(group.off);
    }
    if (tied) result += "~";
    return result;
}

std::string barLine(int bar, const std::vector<Piece>& pieces, const Timeline& time) {
    std::string result = std::to_string(bar) + ":";
    std::int64_t cursor = time.start(bar);
    const auto end = time.start(bar + 1);
    for (const auto& p : pieces) {
        if (p.tick > cursor) result += " r:" + internal::durationName(p.tick - cursor, time.ppq);
        result += " " + p.token;
        cursor = std::max(cursor, p.tick + p.length);
    }
    if (cursor < end) result += " r:" + internal::durationName(end - cursor, time.ppq);
    return result;
}

std::int64_t lastEvent(const midi::Sequence& s) {
    std::int64_t end = 0;
    for (const auto& n : s.notes) end = std::max(end, n.start + n.length);
    for (const auto& c : s.controls) end = std::max(end, c.tick);
    for (const auto& x : s.sysex) end = std::max(end, x.tick);
    return end;
}

std::optional<std::pair<std::string, int>> drumGrid(const std::vector<midi::Note>& notes,
                                                    const Timeline& time,
                                                    const DecompileOptions& options) {
    if (notes.empty() || !options.drumGrids || notes.front().length == 0) return std::nullopt;
    const auto commonLength = notes.front().length;
    std::map<int, int> frequencies;
    for (const auto& n : notes) {
        if (n.length != commonLength || n.offVelocity != 0) return std::nullopt;
        ++frequencies[n.velocity];
    }
    std::vector<std::pair<int, int>> byCount;
    for (const auto& [velocity, count] : frequencies) byCount.emplace_back(count, velocity);
    std::sort(byCount.begin(), byCount.end(), [](auto a, auto b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    const int base = byCount[0].second;
    const int accent = byCount.size() > 1 ? byCount[1].second : 127;
    const int ghost = byCount.size() > 2 ? byCount[2].second : 40;
    auto symbol = [&](int velocity) -> char {
        if (velocity == base) return 'x';
        if (velocity == accent) return 'X';
        if (velocity == ghost) return 'o';
        for (int d = 1; d <= 9; ++d)
            if (velocity == 15 + (d - 1) * 14) return static_cast<char>('0' + d);
        return '\0';
    };
    for (const auto& n : notes)
        if (symbol(n.velocity) == '\0') return std::nullopt;
    const auto& names =
        options.drums && !options.drums->names.empty() ? options.drums->names : internal::gmDrums();
    for (std::string_view gridName : {"s", "e3", "t", "s3", "x"}) {
        bool fraction = false;
        const auto step = internal::duration(gridName, time.ppq, fraction);
        if (!step || *step <= 0) continue;
        std::map<std::pair<int, int>, std::string> rows;
        bool fits = true;
        int lastBar = 0;
        for (const auto& n : notes) {
            const int bar = time.bar(n.start);
            const auto length = internal::barTicks(time.meter(bar), time.ppq);
            const auto offset = n.start - time.start(bar);
            if (length <= 0 || length % *step != 0 || offset % *step != 0) {
                fits = false;
                break;
            }
            const auto index = static_cast<std::size_t>(offset / *step);
            auto& pattern = rows[{bar, n.pitch}];
            if (pattern.empty()) pattern.assign(static_cast<std::size_t>(length / *step), '.');
            if (index >= pattern.size() || pattern[index] != '.') {
                fits = false;
                break;
            }
            pattern[index] = symbol(n.velocity);
            lastBar = std::max(lastBar, bar);
        }
        if (!fits) continue;
        std::string output = "\nvoice drums ch 10 grid " + std::string(gridName) + " len " +
                             internal::durationName(commonLength, time.ppq) + " vel " +
                             std::to_string(base) + " accent " + std::to_string(accent) +
                             " ghost " + std::to_string(ghost) + "\n";
        std::set<int> bars;
        for (const auto& [key, ignored] : rows) {
            (void)ignored;
            bars.insert(key.first);
        }
        for (int bar : bars) {
            std::set<int> done;
            for (const auto& [name, pitch] : names) {
                if (auto it = rows.find({bar, pitch}); it != rows.end()) {
                    output += std::to_string(bar) + ": " + name + " " + it->second + "\n";
                    done.insert(pitch);
                }
            }
            for (const auto& [key, pattern] : rows) {
                if (key.first == bar && !done.contains(key.second))
                    output += std::to_string(bar) + ": n" + std::to_string(key.second) + " " +
                              pattern + "\n";
            }
        }
        return std::pair<std::string, int>{output, lastBar};
    }
    return std::nullopt;
}

}  // namespace

Result<std::int64_t> parseDuration(std::string_view token, int ppq) {
    bool fraction = false;
    const auto ticks = internal::duration(token, ppq, fraction);
    if (!ticks || *ticks == 0)
        return Error(fraction ? "notation.tick_fraction" : "notation.bad_duration",
                     "invalid duration '" + std::string(token) + "'", "",
                     "use w, h, q, e, s, t, x, dots, tuplets, sums such as h+e, or positive Nt "
                     "ticks");
    return *ticks;
}

std::string durationText(std::int64_t ticks, int ppq) { return internal::durationName(ticks, ppq); }

Result<std::string> decompile(const midi::Sequence& source, const DecompileOptions& options) {
    for (const auto& note : source.notes)
        if (note.start < 0 || note.length < 0 ||
            note.length > std::numeric_limits<std::int64_t>::max() - note.start)
            return Error{"notation.invalid_sequence",
                         "Note time is invalid or overflows.",
                         {},
                         "Use non-negative note times whose sum fits in tick time."};
    if (source.ppq < 1 || source.ppq > 32767 || source.length < 0 ||
        source.length < lastEvent(source))
        return Error{"notation.invalid_sequence",
                     "Sequence has invalid PPQ or length.",
                     {},
                     "Use PPQ 1..32767 and length at least the last event end."};
    midi::Sequence s = source;
    s.canonicalize();
    Timeline time{s.ppq, options};
    std::int64_t checkedUntil = 0;
    for (int bar = 1; bar <= 100000; ++bar) {
        const auto meter = time.meter(bar);
        if (meter.numerator < 1 || meter.numerator > 64 || meter.denominator < 1 ||
            meter.denominator > 64 || (meter.denominator & (meter.denominator - 1)) != 0 ||
            internal::barTicks(meter, s.ppq) < 1 ||
            (std::int64_t(s.ppq) * 4 * meter.numerator) % meter.denominator != 0)
            return Error{"notation.tick_fraction",
                         "Meter has an invalid or fractional-tick bar.",
                         {},
                         "Use a valid meter and compatible PPQ throughout the item."};
        checkedUntil += internal::barTicks(meter, s.ppq);
        time.starts.push_back(checkedUntil);
        if (checkedUntil > s.length) break;
    }
    if (checkedUntil < s.length)
        return Error{"notation.invalid_sequence",
                     "Sequence exceeds the notation bar limit.",
                     {},
                     "Use an item of at most 100000 bars."};

    for (const auto& note : s.notes)
        if (time.bar(note.start) > 100000)
            return Error{"notation.invalid_sequence",
                         "Note starts beyond the notation bar limit.",
                         {},
                         "Use note positions in bars 1..100000."};
    for (const auto& control : s.controls)
        if (time.bar(control.tick) > 100000)
            return Error{"notation.invalid_sequence",
                         "Control is beyond the notation bar limit.",
                         {},
                         "Use control positions in bars 1..100000."};
    for (const auto& sx : s.sysex)
        if (time.bar(sx.tick) > 100000)
            return Error{"notation.invalid_sequence",
                         "Sysex is beyond the notation bar limit.",
                         {},
                         "Use sysex positions in bars 1..100000."};

    std::map<int, std::vector<Group>> channels;
    std::map<std::tuple<int, std::int64_t, std::int64_t, int, int>, std::size_t> groupIndex;
    for (const auto& note : s.notes) {
        if (note.start < 0 || note.length < 0 || note.channel > 15 || note.pitch > 127 ||
            note.velocity < 1 || note.velocity > 127 || note.offVelocity > 127)
            return Error{"notation.invalid_sequence",
                         "Sequence has an invalid note.",
                         {},
                         "Check note time, pitch, channel, and velocities."};
        auto& groups = channels[note.channel];
        const auto key = std::tuple{int(note.channel), note.start, note.length, int(note.velocity),
                                    int(note.offVelocity)};
        auto [found, added] = groupIndex.emplace(key, groups.size());
        if (added)
            groups.push_back(
                {note.start, note.length, note.velocity, note.offVelocity, {note.pitch}});
        else
            groups[found->second].pitches.push_back(note.pitch);
    }
    std::string body;
    int lastWrittenBar = 0;
    std::optional<std::pair<std::string, int>> drums;
    if (auto it = channels.find(9); it != channels.end()) {
        std::vector<midi::Note> drumNotes;
        for (const auto& n : s.notes)
            if (n.channel == 9) drumNotes.push_back(n);
        drums = drumGrid(drumNotes, time, options);
        if (drums) {
            body += drums->first;
            lastWrittenBar = drums->second;
        }
    }
    for (auto& [channel, groups] : channels) {
        if (channel == 9 && drums) continue;
        std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
            return std::tie(a.start, a.pitches[0], a.length, a.velocity, a.off) <
                   std::tie(b.start, b.pitches[0], b.length, b.velocity, b.off);
        });
        std::vector<Voice> voices;
        using End = std::pair<std::int64_t, std::size_t>;
        std::priority_queue<End, std::vector<End>, std::greater<End>> busy;
        std::set<std::size_t> freeVoices;
        for (auto& group : groups) {
            std::sort(group.pitches.begin(), group.pitches.end());
            while (!busy.empty() && busy.top().first <= group.start) {
                freeVoices.insert(busy.top().second);
                busy.pop();
            }
            std::size_t index = voices.size();
            if (freeVoices.empty())
                voices.push_back({});
            else {
                index = *freeVoices.begin();
                freeVoices.erase(freeVoices.begin());
            }
            auto& voice = voices[index];
            voice.groups.push_back(group);
            voice.cursor = group.start + group.length;
            busy.emplace(voice.cursor, index);
        }
        for (std::size_t vi = 0; vi < voices.size(); ++vi) {
            const auto& voice = voices[vi];
            std::map<int, int> frequencies;
            for (const auto& group : voice.groups) ++frequencies[group.velocity];
            int baseVelocity = 96, count = -1;
            for (const auto& [velocity, frequency] : frequencies)
                if (frequency > count) {
                    count = frequency;
                    baseVelocity = velocity;
                }
            body += "\nvoice c" + std::to_string(channel + 1) + "v" + std::to_string(vi + 1) +
                    " ch " + std::to_string(channel + 1) + " vel " + std::to_string(baseVelocity) +
                    "\n";
            std::map<int, std::vector<Piece>> bars;
            for (const auto& group : voice.groups) {
                auto at = group.start;
                auto remaining = group.length;
                if (remaining == 0) {
                    bars[time.bar(at)].push_back(
                        {at, 0, noteToken(group, 0, s.ppq, baseVelocity, false, options.key)});
                    continue;
                }
                while (remaining > 0) {
                    const int bar = time.bar(at);
                    const auto available = time.start(bar + 1) - at;
                    if (available <= 0)
                        return Error{"notation.invalid_sequence",
                                     "Invalid meter map.",
                                     {},
                                     "Use positive whole-tick bar lengths."};
                    const auto take = std::min(remaining, available);
                    bars[bar].push_back({at, take,
                                         noteToken(group, take, s.ppq, baseVelocity,
                                                   remaining > take, options.key)});
                    remaining -= take;
                    at += take;
                }
            }
            struct BarOutput {
                int bar;
                int source;
                std::string text;
            };
            std::vector<BarOutput> outputs;
            std::map<std::pair<int, std::string>, int> earliest;
            for (auto& [bar, pieces] : bars) {
                std::stable_sort(pieces.begin(), pieces.end(),
                                 [](const Piece& a, const Piece& b) { return a.tick < b.tick; });
                const std::string text = barLine(bar, pieces, time);
                const auto key =
                    std::pair{time.meter(bar).numerator * 128 + time.meter(bar).denominator,
                              text.substr(text.find(':') + 1)};
                const auto [it, inserted] = earliest.emplace(key, bar);
                outputs.push_back({bar, inserted ? 0 : it->second, text});
                lastWrittenBar = std::max(lastWrittenBar, bar);
            }
            for (std::size_t i = 0; i < outputs.size();) {
                const auto& current = outputs[i];
                if (current.source == 0) {
                    body += current.text + "\n";
                    ++i;
                    continue;
                }
                std::size_t end = i + 1;
                int stride = 0;
                if (end < outputs.size()) stride = outputs[end].source - current.source;
                while (end < outputs.size() && outputs[end].source > 0 &&
                       outputs[end].bar == outputs[end - 1].bar + 1 &&
                       (stride == 0 || stride == 1) &&
                       outputs[end].source == outputs[end - 1].source + stride)
                    ++end;
                if (end == i + 1) {
                    body +=
                        std::to_string(current.bar) + " = " + std::to_string(current.source) + "\n";
                } else {
                    body += std::to_string(current.bar) + "-" +
                            std::to_string(outputs[end - 1].bar) + " = " +
                            std::to_string(current.source);
                    if (stride == 1) body += "-" + std::to_string(outputs[end - 1].source);
                    body += "\n";
                }
                i = end;
            }
        }
    }

    using Key = std::tuple<midi::ControlKind, int, int>;
    std::map<Key, std::vector<midi::Control>> lanes;
    std::map<int, std::vector<midi::Control>> programs;
    for (const auto& c : s.controls) {
        const bool bend = c.kind == midi::ControlKind::PitchBend;
        if (c.tick < 0 || c.channel > 15 || c.number > 127 ||
            static_cast<unsigned>(c.kind) > static_cast<unsigned>(midi::ControlKind::PitchBend) ||
            c.value < (bend ? -8192 : 0) || c.value > (bend ? 8191 : 127) ||
            ((c.kind == midi::ControlKind::ChannelPressure ||
              c.kind == midi::ControlKind::PitchBend ||
              c.kind == midi::ControlKind::ProgramChange) &&
             c.number != 0))
            return Error{"notation.invalid_sequence",
                         "Sequence has an invalid control.",
                         {},
                         "Check controller ranges."};
        if (c.kind == midi::ControlKind::ProgramChange)
            programs[c.channel].push_back(c);
        else
            lanes[{c.kind, c.channel, c.number}].push_back(c);
    }
    std::vector<Key> laneKeys;
    for (const auto& [key, ignored] : lanes) {
        (void)ignored;
        laneKeys.push_back(key);
    }
    auto rank = [](midi::ControlKind kind) {
        switch (kind) {
            case midi::ControlKind::CC:
                return 0;
            case midi::ControlKind::PitchBend:
                return 1;
            case midi::ControlKind::ChannelPressure:
                return 2;
            case midi::ControlKind::PolyPressure:
                return 3;
            case midi::ControlKind::ProgramChange:
                return 4;
        }
        return 5;
    };
    std::sort(laneKeys.begin(), laneKeys.end(), [&](const Key& a, const Key& b) {
        const auto [ak, ac, an] = a;
        const auto [bk, bc, bn] = b;
        return std::tuple{rank(ak), an, ac} < std::tuple{rank(bk), bn, bc};
    });
    for (const auto& key : laneKeys) {
        const auto& points = lanes.at(key);
        const auto [kind, channel, number] = key;
        body += "\nlane ";
        switch (kind) {
            case midi::ControlKind::CC:
                body += "cc " + std::to_string(number);
                break;
            case midi::ControlKind::PitchBend:
                body += "bend";
                break;
            case midi::ControlKind::ChannelPressure:
                body += "pressure";
                break;
            case midi::ControlKind::PolyPressure:
                body += "poly " + internal::pitchName(number, options.key);
                break;
            case midi::ControlKind::ProgramChange:
                break;
        }
        body += " ch " + std::to_string(channel + 1) + "\n";
        for (const auto& point : points)
            body += time.position(point.tick) + " " + std::to_string(point.value) + "\n";
    }
    std::map<int, std::vector<midi::Sysex>> sysex;
    for (const auto& sx : s.sysex) {
        if (sx.tick < 0 || sx.bytes.size() < 2 || sx.bytes.front() != 0xf0 ||
            sx.bytes.back() != 0xf7)
            return Error{"notation.invalid_sequence",
                         "Sequence has invalid sysex framing or time.",
                         {},
                         "Use non-negative time and include F0 and F7."};
        sysex[0].push_back(sx);
    }
    for (int channel = 0; channel < 16; ++channel) {
        if (!programs.contains(channel) && !sysex.contains(channel)) continue;
        body += "\nevents ch " + std::to_string(channel + 1) + "\n";
        for (const auto& c : programs[channel])
            body += time.position(c.tick) + " program " + std::to_string(c.value) + "\n";
        for (const auto& sx : sysex[channel]) {
            body += time.position(sx.tick) + " sysex";
            for (auto byte : sx.bytes) {
                std::ostringstream stream;
                stream << ' ' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<int>(byte);
                body += stream.str();
            }
            body += "\n";
        }
    }

    int lastPositionBar = lastWrittenBar;
    for (const auto& c : s.controls) lastPositionBar = std::max(lastPositionBar, time.bar(c.tick));
    for (const auto& sx : s.sysex) lastPositionBar = std::max(lastPositionBar, time.bar(sx.tick));
    const auto inferredLength = time.start(lastPositionBar + 1);
    std::string header = "notation 1\nppq " + std::to_string(s.ppq) + "\n";
    if (!options.meterAt) header += "meter 4/4\n";
    if (s.length != inferredLength) header += "length " + time.length(s.length) + "\n";
    return header + body;
}

}  // namespace supple::notation
