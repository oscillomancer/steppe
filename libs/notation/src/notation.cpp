// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/notation/notation.h>

#include "internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace supple::notation {
namespace {

struct Token {
    std::string text;
    int line;
    int column;
};
struct Statement {
    std::vector<Token> tokens;
};

std::vector<Statement> lex(std::string_view input) {
    std::vector<Statement> result;
    Statement statement;
    int line = 1, column = 1;
    std::size_t i = 0;
    while (i < input.size()) {
        const char c = input[i];
        if (c == '#' || c == ';' || c == '\n') {
            if (!statement.tokens.empty()) result.push_back(std::move(statement));
            statement = {};
            if (c == '#') {
                while (i < input.size() && input[i] != '\n') {
                    ++i;
                    ++column;
                }
                continue;
            }
            ++i;
            if (c == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            ++column;
            continue;
        }
        const int start = column;
        const auto begin = i;
        int bracket = 0;
        do {
            const char next = input[i];
            if (next == '[' || next == '{') ++bracket;
            if (next == ']' || next == '}') --bracket;
            ++i;
            ++column;
        } while (i < input.size() &&
                 (bracket > 0 || (input[i] != ' ' && input[i] != '\t' && input[i] != '\r' &&
                                  input[i] != '\n' && input[i] != ';')));
        statement.tokens.push_back({std::string(input.substr(begin, i - begin)), line, start});
    }
    if (!statement.tokens.empty()) result.push_back(std::move(statement));
    return result;
}

std::size_t distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> older(b.size() + 1), previous(b.size() + 1), row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) previous[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            row[j] = std::min(
                {previous[j] + 1, row[j - 1] + 1, previous[j - 1] + (a[i - 1] != b[j - 1])});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                row[j] = std::min(row[j], older[j - 2] + 1);
        }
        older.swap(previous);
        previous.swap(row);
    }
    return previous[b.size()];
}

// Notation hints use case-sensitive transpositions and declaration-order ties.
std::string closest(std::string_view input, const std::vector<std::string>& choices) {
    std::string answer;
    std::size_t best = 100000;
    for (const auto& choice : choices) {
        const auto difference = input.size() > choice.size() ? input.size() - choice.size()
                                                             : choice.size() - input.size();
        if (difference >= best) continue;
        const auto cost = distance(input, choice);
        if (cost < best) {
            best = cost;
            answer = choice;
        }
    }
    return answer;
}

struct Bar {
    std::vector<std::vector<Token>> rows;
    int line = 0;
};
struct Voice {
    std::string name;
    int channel = 0, velocity = 96, accent = 127, ghost = 40, octave = 3;
    bool grid = false;
    std::int64_t step = 0, hitLength = 0;
    int lastBar = 0;
    std::map<int, Bar> bars;
};
struct Lane {
    midi::ControlKind kind = midi::ControlKind::CC;
    int channel = 0, number = 0;
    double range = 2.0;
    std::vector<Statement> points;
};
struct Events {
    int channel = 0;
    std::vector<Statement> points;
};

class Compiler {
  public:
    Compiler(std::string_view source, const CompileContext& context)
        : context_(context), lines_(lex(source)) {
        result_.sequence.ppq = context.ppq.value_or(960);
    }

    Result<Compiled> run() {
        if (result_.sequence.ppq < 1 || result_.sequence.ppq > 32767) {
            add("syntax", "Invalid host PPQ.", Token{"ppq", 1, 1}, "Use PPQ 1..32767.");
            return errorResult();
        }
        parseStructure();
        if (!errors_.empty()) return errorResult();
        const int meterLimit = std::max(maxBar_, length_ ? length_->first + 1 : 1);
        for (int bar = 1; bar <= meterLimit; ++bar) {
            const auto m = meter(bar);
            if (!validMeter(m) ||
                (std::int64_t(result_.sequence.ppq) * 4 * m.numerator) % m.denominator != 0) {
                add("tick_fraction",
                    "Bar " + std::to_string(bar) + " has a fractional or invalid meter length.",
                    Token{"meter", 1, 1},
                    "Use a valid meter and a PPQ that makes whole-tick bars.");
                break;
            }
        }
        if (!errors_.empty()) return errorResult();
        for (auto& voice : voices_) compileVoice(voice);
        for (const auto& lane : lanes_) compileLane(lane);
        for (const auto& events : events_) compileEvents(events);
        if (!errors_.empty()) return errorResult();
        const std::int64_t eventEnd = lastEventEnd();
        if (length_) {
            const auto [bars, ticks] = *length_;
            if (ticks >= internal::barTicks(meter(bars + 1), result_.sequence.ppq))
                add("syntax", "Length tick remainder must be less than one bar.", lengthToken_,
                    "Increase the whole-bar count and reduce the tick remainder.");
            if (!errors_.empty()) return errorResult();
            const auto start = barStart(bars + 1);
            if (!errors_.empty()) return errorResult();
            if (ticks > std::numeric_limits<std::int64_t>::max() - start) {
                add("bad_duration", "Item length overflows tick time.", lengthToken_,
                    "Use a shorter pickup or length.");
                return errorResult();
            }
            result_.sequence.length = start + ticks;
        } else {
            result_.sequence.length = barStart(maxBar_ + 1);
            if (!errors_.empty()) return errorResult();
            while (result_.sequence.length < eventEnd && maxBar_ < 100000) {
                const auto span = internal::barTicks(meter(++maxBar_), result_.sequence.ppq);
                if (span > std::numeric_limits<std::int64_t>::max() - result_.sequence.length) {
                    add("bad_duration", "Item length overflows tick time.", lengthToken_,
                        "Use a shorter pickup or duration.");
                    return errorResult();
                }
                result_.sequence.length += span;
            }
        }
        if (result_.sequence.length < eventEnd)
            add("length_short", "Item length ends before the last event.", lengthToken_,
                "Increase the length header to include the last event.");
        if (!errors_.empty()) return errorResult();
        for (int b = 1; b <= maxBar_; ++b) result_.barMeters.push_back(meter(b));
        result_.sequence.canonicalize();
        return std::move(result_);
    }

  private:
    const CompileContext& context_;
    std::vector<Statement> lines_;
    Compiled result_;
    std::vector<Error> errors_;
    std::vector<Voice> voices_;
    std::vector<Lane> lanes_;
    std::vector<Events> events_;
    midi::Meter firstMeter_{};
    std::map<int, midi::Meter> meterChanges_;
    std::optional<std::pair<int, std::int64_t>> length_;
    Token lengthToken_{"length", 1, 1};
    int maxBar_ = 0;
    int lastMeterBar_ = 0;

    void add(std::string code, std::string message, const Token& at, std::string hint) {
        if (errors_.size() >= 50) return;
        Error error{"notation." + code, std::move(message),
                    std::to_string(at.line) + ":" + std::to_string(at.column), std::move(hint)};
        error.loc = SourceLoc{at.line, at.column};
        errors_.push_back(std::move(error));
    }
    Result<Compiled> errorResult() {
        if (errors_.size() == 1) return std::move(errors_.front());
        Error all{"notation.invalid",
                  "The notation has multiple errors.",
                  {},
                  "Fix each listed error and compile again."};
        all.details = std::move(errors_);
        return all;
    }
    static bool validMeter(midi::Meter m) {
        return m.numerator >= 1 && m.numerator <= 64 && m.denominator >= 1 && m.denominator <= 64 &&
               (m.denominator & (m.denominator - 1)) == 0;
    }
    midi::Meter meter(int bar) const {
        if (context_.meterAt) return context_.meterAt(bar);
        auto it = meterChanges_.upper_bound(bar);
        if (it == meterChanges_.begin()) return firstMeter_;
        return std::prev(it)->second;
    }
    std::int64_t barStart(int bar) {
        if (bar <= 0) return 0;
        if (barStarts_.empty()) barStarts_.push_back(result_.pickupTicks);
        const auto count = static_cast<std::size_t>(std::min(bar, 100001));
        while (barStarts_.size() < count) {
            const auto span = internal::barTicks(meter(static_cast<int>(barStarts_.size())),
                                                 result_.sequence.ppq);
            if (span > std::numeric_limits<std::int64_t>::max() - barStarts_.back()) {
                add("bad_duration", "Bar position overflows tick time.", Token{"pickup", 1, 1},
                    "Use a shorter pickup.");
                return 0;
            }
            barStarts_.push_back(barStarts_.back() + span);
        }
        return barStarts_[count - 1];
    }
    std::vector<std::int64_t> barStarts_;

    std::int64_t lastEventEnd() const {
        std::int64_t end = 0;
        for (const auto& note : result_.sequence.notes)
            end = std::max(end, note.start + note.length);
        for (const auto& c : result_.sequence.controls) end = std::max(end, c.tick);
        for (const auto& sx : result_.sequence.sysex) end = std::max(end, sx.tick);
        return end;
    }
    std::optional<int> channel(const Token& token) {
        auto value = internal::integer(token.text);
        if (!value || *value < 1 || *value > 16) {
            add("channel_range", "Channel must be 1..16.", token, "Use ch 1 through ch 16.");
            return std::nullopt;
        }
        return static_cast<int>(*value - 1);
    }
    std::optional<int> velocity(const Token& token) {
        auto value = internal::integer(token.text);
        if (!value || *value < 1 || *value > 127) {
            add("velocity_range", "Velocity must be 1..127.", token, "Use a value from 1 to 127.");
            return std::nullopt;
        }
        return static_cast<int>(*value);
    }
    std::optional<std::int64_t> duration(const Token& token, bool allowZero = false) {
        bool fraction = false;
        auto ticks = internal::duration(token.text, result_.sequence.ppq, fraction);
        if (!ticks || (!allowZero && *ticks == 0)) {
            add(fraction ? "tick_fraction" : "bad_duration",
                "Invalid duration '" + token.text + "'.", token,
                "Use w, h, q, e, s, t, x, dots, tuplets, sums, or positive Nt ticks.");
            return std::nullopt;
        }
        return ticks;
    }
    std::optional<int> pitch(const Token& token) {
        auto value = internal::pitch(token.text);
        if (!value) {
            bool range = false;
            if (token.text.starts_with('n')) {
                auto n = internal::integer(std::string_view(token.text).substr(1));
                range = n && (*n < 0 || *n > 127);
            } else if (!token.text.empty() &&
                       std::string_view("ABCDEFGabcdefg").find(token.text[0]) !=
                           std::string_view::npos) {
                std::size_t i = 1;
                while (i < token.text.size() && (token.text[i] == '#' || token.text[i] == 'b')) ++i;
                range = internal::integer(std::string_view(token.text).substr(i)).has_value();
            }
            add(range ? "pitch_range" : "bad_pitch", "Invalid pitch '" + token.text + "'.", token,
                "Use C4, F#3, Bb2, or n60 (MIDI 0..127).");
        }
        return value;
    }
    std::optional<std::pair<int, int>> barRange(const Token& token, bool colon) {
        std::string_view text = token.text;
        if (colon) {
            if (text.empty() || text.back() != ':') return std::nullopt;
            text.remove_suffix(1);
        }
        const auto dash = text.find('-');
        auto a = internal::integer(text.substr(0, dash));
        auto b = dash == std::string_view::npos ? a : internal::integer(text.substr(dash + 1));
        if (!a || !b || *a < 0 || (*a == 0 && result_.pickupTicks == 0) || *b < *a || *b > 100000)
            return std::nullopt;
        return std::pair<int, int>{static_cast<int>(*a), static_cast<int>(*b)};
    }

    void parseStructure() {
        enum class Section { Header, Voice, Lane, Events } section = Section::Header;
        Voice* currentVoice = nullptr;
        Lane* currentLane = nullptr;
        Events* currentEvents = nullptr;
        for (const auto& line : lines_) {
            const auto& t = line.tokens;
            const auto& head = t[0].text;
            const bool barLike =
                !head.empty() && head.front() >= '0' && head.front() <= '9' && head.back() == ':';
            if (head == "notation") {
                if (t.size() != 2 || t[1].text != "1")
                    add("syntax", "Unsupported notation version.", t[0], "Write 'notation 1'.");
                continue;
            }
            if (head == "ppq") {
                auto n = t.size() == 2 ? internal::integer(t[1].text) : std::nullopt;
                if (!n || *n < 1 || *n > 32767)
                    add("syntax", "Invalid PPQ.", t[0], "Use ppq 960 or another positive integer.");
                else if (context_.ppq && *context_.ppq != *n)
                    add("syntax", "PPQ conflicts with host PPQ.", t[0], "Use the host's PPQ.");
                else
                    result_.sequence.ppq = static_cast<int>(*n);
                continue;
            }
            if (head == "pickup") {
                if (t.size() != 2) {
                    add("syntax", "Pickup needs a duration.", t[0], "Use pickup q or pickup 240t.");
                    continue;
                }
                auto ticks = duration(t[1]);
                if (ticks) result_.pickupTicks = *ticks;
                continue;
            }
            if (head == "tempo") {
                auto n = t.size() == 2 ? internal::number(t[1].text) : std::nullopt;
                if (!n || *n <= 0)
                    add("syntax", "Invalid tempo.", t[0], "Use a positive BPM, such as tempo 120.");
                else
                    result_.tempo = *n;
                continue;
            }
            if (head == "length") {
                lengthToken_ = t[0];
                if (t.size() != 2) {
                    add("syntax", "Invalid length header.", t[0], "Use length 8 or length 8+480t.");
                    continue;
                }
                const auto plus = t[1].text.find('+');
                auto bars = internal::integer(std::string_view(t[1].text).substr(0, plus));
                std::optional<std::int64_t> extra = 0;
                if (plus != std::string::npos) {
                    const auto tail = std::string_view(t[1].text).substr(plus + 1);
                    extra = tail.size() > 1 && tail.back() == 't'
                                ? internal::integer(tail.substr(0, tail.size() - 1))
                                : std::nullopt;
                }
                if (!bars || *bars < 0 || *bars > 100000 || !extra || *extra < 0) {
                    add("syntax", "Invalid item length.", t[1], "Use length B or length B+Nt.");
                    continue;
                }
                length_ = {static_cast<int>(*bars), *extra};
                continue;
            }
            if (head == "meter") {
                const auto slash = t.size() >= 2 ? t[1].text.find('/') : std::string::npos;
                auto n = slash != std::string::npos
                             ? internal::integer(std::string_view(t[1].text).substr(0, slash))
                             : std::nullopt;
                auto d = slash != std::string::npos
                             ? internal::integer(std::string_view(t[1].text).substr(slash + 1))
                             : std::nullopt;
                if (!n || !d || *n < 1 || *d < 1 || *n > 64 || *d > 64 ||
                    !validMeter({static_cast<int>(*n), static_cast<int>(*d)})) {
                    add("syntax", "Invalid meter.", t[0],
                        "Use meter N/D with a power-of-two denominator.");
                    continue;
                }
                midi::Meter value{static_cast<int>(*n), static_cast<int>(*d)};
                int atBar = 1;
                if (t.size() == 4 && t[2].text == "at") {
                    auto at = internal::integer(t[3].text);
                    if (!at || *at < 1 || *at > 100000) {
                        add("syntax", "Invalid meter bar.", t[3], "Use meter 3/4 at 5.");
                        continue;
                    }
                    atBar = static_cast<int>(*at);
                } else if (t.size() != 2) {
                    add("syntax", "Invalid meter change.", t[0], "Use meter 3/4 at 5.");
                    continue;
                }
                if (atBar <= lastMeterBar_)
                    add("meter_order", "Meter changes are out of order.", t[0],
                        "Write meter changes in increasing bar order.");
                lastMeterBar_ = atBar;
                if (atBar == 1) {
                    firstMeter_ = value;
                } else
                    meterChanges_[atBar] = value;
                if (context_.meterAt && value != context_.meterAt(atBar))
                    add("meter_mismatch", "Meter differs from host meter.", t[0],
                        "Use the host meter at the specified bar.");
                if ((std::int64_t(result_.sequence.ppq) * 4 * value.numerator) %
                        value.denominator !=
                    0)
                    add("tick_fraction", "Meter produces a fractional tick bar.", t[1],
                        "Use a PPQ divisible by this meter denominator.");
                continue;
            }
            if (head == "voice") {
                if (t.size() < 2) {
                    add("syntax", "Voice needs a name.", t[0], "Use voice bass ch 1.");
                    continue;
                }
                const auto& name = t[1].text;
                const bool valid =
                    !name.empty() && name[0] >= 'a' && name[0] <= 'z' &&
                    std::all_of(name.begin(), name.end(), [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                    });
                if (!valid)
                    add("syntax", "Invalid voice name.", t[1],
                        "Use lowercase letters, digits, and underscores.");
                for (const auto& v : voices_)
                    if (v.name == name)
                        add("duplicate_voice", "Voice name is repeated.", t[1],
                            "Give each voice a distinct name.");
                voices_.push_back({});
                currentVoice = &voices_.back();
                currentVoice->name = name;
                section = Section::Voice;
                for (std::size_t i = 2; i < t.size();) {
                    const auto& key = t[i];
                    if (i + 1 >= t.size()) {
                        add("syntax", "Voice option needs a value.", key,
                            "Add a value after the option.");
                        break;
                    }
                    const auto& value = t[i + 1];
                    if (key.text == "ch") {
                        if (auto ch = channel(value)) currentVoice->channel = *ch;
                    } else if (key.text == "vel") {
                        if (auto v = velocity(value)) currentVoice->velocity = *v;
                    } else if (key.text == "accent") {
                        if (auto v = velocity(value)) currentVoice->accent = *v;
                    } else if (key.text == "ghost") {
                        if (auto v = velocity(value)) currentVoice->ghost = *v;
                    } else if (key.text == "octave") {
                        auto n = internal::integer(value.text);
                        if (!n || *n < -1 || *n > 8)
                            add("syntax", "Chord octave is invalid.", value,
                                "Use octave -1 through 8.");
                        else
                            currentVoice->octave = static_cast<int>(*n);
                    } else if (key.text == "grid" || key.text == "len") {
                        auto ticks = duration(value);
                        if (ticks) {
                            if (key.text == "grid") {
                                currentVoice->grid = true;
                                currentVoice->step = *ticks;
                            } else
                                currentVoice->hitLength = *ticks;
                        }
                    } else {
                        add("unknown_keyword", "Unknown voice option '" + key.text + "'.", key,
                            "Did you mean '" +
                                closest(key.text,
                                        {"ch", "vel", "grid", "len", "accent", "ghost", "octave"}) +
                                "'?");
                    }
                    i += 2;
                }
                if (currentVoice->grid && currentVoice->hitLength == 0)
                    currentVoice->hitLength = currentVoice->step;
                continue;
            }
            if (head == "lane") {
                lanes_.push_back({});
                currentLane = &lanes_.back();
                section = Section::Lane;
                if (t.size() < 2) {
                    add("syntax", "Lane needs a kind.", t[0],
                        "Use lane cc 74, lane bend, lane pressure, or lane poly C4.");
                    continue;
                }
                std::size_t i = 2;
                if (t[1].text == "cc" || t[1].text == "poly") {
                    if (i >= t.size()) {
                        add("syntax", "Lane needs a number or pitch.", t[1],
                            "Add a CC number or poly pitch.");
                        continue;
                    }
                    if (t[1].text == "poly") {
                        currentLane->kind = midi::ControlKind::PolyPressure;
                        if (auto p = pitch(t[i])) currentLane->number = *p;
                    } else {
                        auto n = internal::integer(t[i].text);
                        if (n && *n >= 0 && *n <= 127)
                            currentLane->number = static_cast<int>(*n);
                        else if (auto it = internal::ccNames().find(t[i].text);
                                 it != internal::ccNames().end())
                            currentLane->number = it->second;
                        else
                            add("syntax", "Unknown CC name or number.", t[i],
                                "Use CC 0..127 or a named CC such as cutoff.");
                    }
                    ++i;
                } else if (t[1].text == "bend")
                    currentLane->kind = midi::ControlKind::PitchBend;
                else if (t[1].text == "pressure")
                    currentLane->kind = midi::ControlKind::ChannelPressure;
                else
                    add("unknown_keyword", "Unknown lane kind.", t[1],
                        "Use cc, bend, pressure, or poly.");
                while (i + 1 < t.size()) {
                    if (t[i].text == "ch") {
                        if (auto ch = channel(t[i + 1])) currentLane->channel = *ch;
                    } else if (t[i].text == "range") {
                        auto n = internal::number(t[i + 1].text);
                        if (!n || *n <= 0)
                            add("syntax", "Invalid bend range.", t[i + 1],
                                "Use a positive semitone range.");
                        else
                            currentLane->range = *n;
                    } else
                        add("unknown_keyword", "Unknown lane option.", t[i], "Use ch or range.");
                    i += 2;
                }
                if (i < t.size())
                    add("syntax", "Lane option needs a value.", t[i], "Add its value.");
                continue;
            }
            if (head == "events") {
                events_.push_back({});
                currentEvents = &events_.back();
                section = Section::Events;
                if (t.size() == 3 && t[1].text == "ch") {
                    if (auto ch = channel(t[2])) currentEvents->channel = *ch;
                } else if (t.size() != 1)
                    add("syntax", "Invalid events header.", t[0], "Use events or events ch N.");
                continue;
            }
            if (section == Section::Header && barLike) {
                voices_.push_back({});
                currentVoice = &voices_.back();
                currentVoice->name = "main";
                section = Section::Voice;
            }
            if (section == Section::Voice && currentVoice) {
                if (t.size() >= 3 && t[1].text == "=") {
                    auto dest = barRange(t[0], false), source = barRange(t[2], false);
                    if (!dest || !source || t.size() != 3) {
                        add("syntax", "Invalid copy range.", t[0], "Use 5-8 = 1-4.");
                        continue;
                    }
                    if (dest->first <= currentVoice->lastBar)
                        add("bar_order", "Copied bars are out of order.", t[0],
                            "Use increasing bar numbers.");
                    for (int b = dest->first; b <= dest->second; ++b) {
                        const int src = source->first +
                                        (b - dest->first) % (source->second - source->first + 1);
                        const auto it = currentVoice->bars.find(src);
                        if (it == currentVoice->bars.end())
                            add("copy_source", "Copy source bar was not written.", t[2],
                                "Write the source bar before copying it.");
                        else
                            currentVoice->bars[b] = it->second;
                    }
                    currentVoice->lastBar = dest->second;
                    maxBar_ = std::max(maxBar_, dest->second);
                    continue;
                }
                auto range = barRange(t[0], true);
                if (!range) {
                    add(t[0].text == "0:" && result_.pickupTicks == 0 ? "no_pickup" : "syntax",
                        "Expected a numbered bar.", t[0],
                        "Use pickup DUR before bar 0, or N: followed by events.");
                    continue;
                }
                if (currentVoice->grid) {
                    if (range->first < currentVoice->lastBar)
                        add("bar_order", "Grid bars are out of order.", t[0],
                            "Use increasing bar numbers.");
                    if (t.size() < 3) {
                        add("syntax", "Grid row needs an instrument and pattern.", t[0],
                            "Add a step pattern.");
                        continue;
                    }
                    std::string pattern;
                    for (std::size_t i = 2; i < t.size(); ++i) pattern += t[i].text;
                    std::size_t offset = 0;
                    for (int b = range->first; b <= range->second; ++b) {
                        const auto barLength =
                            b == 0 ? result_.pickupTicks
                                   : internal::barTicks(meter(b), result_.sequence.ppq);
                        const auto count =
                            currentVoice->step > 0 ? barLength / currentVoice->step : 0;
                        auto& dest = currentVoice->bars[b];
                        dest.line = t[0].line;
                        Token stepToken{pattern.substr(std::min(offset, pattern.size()),
                                                       static_cast<std::size_t>(count)),
                                        t[0].line, t.size() > 2 ? t[2].column : t[0].column};
                        dest.rows.push_back({t[1], stepToken});
                        offset += static_cast<std::size_t>(count);
                    }
                    if (offset != pattern.size())
                        add("grid_steps",
                            "Grid row has " + std::to_string(pattern.size()) + " steps.", t.back(),
                            "Use exactly " + std::to_string(offset) + " steps.");
                    currentVoice->lastBar = std::max(currentVoice->lastBar, range->second);
                } else {
                    int bar = range->first;
                    if (range->first != range->second)
                        add("syntax", "A melodic bar line names one bar.", t[0],
                            "Use N: or a copy line.");
                    if (bar <= currentVoice->lastBar &&
                        (bar != 0 || currentVoice->bars.contains(0)))
                        add(currentVoice->bars.contains(bar) ? "bar_overlap" : "bar_order",
                            "Bar appears twice or out of order.", t[0],
                            "Write each bar once in increasing order.");
                    std::vector<Token> row;
                    for (std::size_t i = 1; i <= t.size(); ++i) {
                        if (bar > 100000) {
                            add("syntax", "Inline bar exceeds the supported bar range.", t[i - 1],
                                "Use bar numbers up to 100000.");
                            break;
                        }
                        if (i == t.size() || t[i].text == "|") {
                            currentVoice->bars[bar].rows.push_back(row);
                            currentVoice->bars[bar].line = t[0].line;
                            row.clear();
                            ++bar;
                        } else if (i > 1 && t[i - 1].text == "|" && t[i].text.ends_with(':')) {
                            // A number right after `|` is tolerated when it names the
                            // bar the pipe starts, so `1: ... | 2: ...` reads naturally.
                            const auto written = internal::integer(
                                std::string_view(t[i].text).substr(0, t[i].text.size() - 1));
                            if (written && *written == bar) continue;
                            if (written)
                                add("bar_after_pipe",
                                    "Bar " + std::to_string(*written) +
                                        " follows a pipe where bar " + std::to_string(bar) +
                                        " comes next.",
                                    t[i],
                                    "Drop the number after `|` (bar " + std::to_string(bar) +
                                        " continues there), or start a new line `" +
                                        std::to_string(*written) + ": ...`.");
                            else
                                row.push_back(t[i]);
                        } else
                            row.push_back(t[i]);
                    }
                    currentVoice->lastBar = bar - 1;
                }
                maxBar_ = std::max(maxBar_, currentVoice->lastBar);
                continue;
            }
            if (barLike) {
                add("syntax", "Numbered bar needs a voice section.", t[0],
                    "Add a voice NAME line before this bar.");
                continue;
            }
            if (section == Section::Lane && currentLane)
                currentLane->points.push_back(line);
            else if (section == Section::Events && currentEvents)
                currentEvents->points.push_back(line);
            else
                add("unknown_keyword", "Unknown statement '" + head + "'.", t[0],
                    "Did you mean '" +
                        closest(head, {"notation", "meter", "ppq", "tempo", "length", "voice",
                                       "lane", "events"}) +
                        "'?");
        }
    }

    std::optional<std::pair<int, std::int64_t>> position(const Token& token) {
        const std::string_view s = token.text;
        const auto dot = s.find('.');
        const auto plus = s.find('+');
        auto bar =
            dot == std::string_view::npos ? std::nullopt : internal::integer(s.substr(0, dot));
        auto beat = dot == std::string_view::npos
                        ? std::nullopt
                        : internal::integer(s.substr(
                              dot + 1, plus == std::string_view::npos ? plus : plus - dot - 1));
        std::optional<std::int64_t> offset = 0;
        if (plus != std::string_view::npos) {
            const auto tail = s.substr(plus + 1);
            offset = tail.size() > 1 && tail.back() == 't'
                         ? internal::integer(tail.substr(0, tail.size() - 1))
                         : std::nullopt;
        }
        if (!bar || *bar < 1 || *bar > 100000 || !beat || !offset || *offset < 0) {
            add("position", "Invalid bar.beat position.", token,
                "Use a position such as 2.3+120t.");
            return std::nullopt;
        }
        const auto m = meter(static_cast<int>(*bar));
        if (!validMeter(m) || *beat < 1 || *beat > m.numerator) {
            add("position", "Beat exceeds this bar's meter.", token,
                "Choose a beat within the meter.");
            return std::nullopt;
        }
        const auto beatNumerator = (*beat - 1) * std::int64_t(result_.sequence.ppq) * 4;
        if (beatNumerator % m.denominator != 0) {
            add("tick_fraction", "Position falls on a fractional tick.", token,
                "Use a beat with a whole-tick onset, or use beat 1 with a tick offset.");
            return std::nullopt;
        }
        const auto beatOffset = beatNumerator / m.denominator;
        if (*offset >= internal::barTicks(m, result_.sequence.ppq) - beatOffset) {
            add("position", "Tick offset exceeds this bar.", token,
                "Use an offset that stays within the bar.");
            return std::nullopt;
        }
        const auto within = beatOffset + *offset;
        maxBar_ = std::max(maxBar_, static_cast<int>(*bar));
        const auto start = barStart(static_cast<int>(*bar));
        if (within > std::numeric_limits<std::int64_t>::max() - start) {
            add("position", "Position overflows tick time.", token, "Use a shorter pickup.");
            return std::nullopt;
        }
        return std::pair<int, std::int64_t>{static_cast<int>(*bar), start + within};
    }

    std::optional<std::vector<int>> chord(std::string_view text, int octave, const Token& at) {
        const auto slash = text.find('/');
        const auto symbol = text.substr(0, slash);
        if (symbol.empty()) return std::nullopt;
        // The grammar, the quality table and the voicing are harmony's; the
        // warnings below keep the compiler's codes and wording.
        auto parsed = harmony::parseChord(text);
        auto pitches = parsed ? harmony::chordPitches(*parsed, octave)
                              : Result<std::vector<int>>(parsed.error());
        if (pitches) return *pitches;
        const std::string& code = pitches.error().code;
        if (code == "chord.bad_root") {
            add("bad_pitch", "Invalid chord root.", at, "Use a root such as C or Bb.");
        } else if (code == "chord.unknown_quality") {
            std::size_t i = 1;
            if (i < symbol.size() && (symbol[i] == '#' || symbol[i] == 'b')) ++i;
            const std::string quality(symbol.substr(i));
            std::vector<std::string> names;
            for (const auto& [name, ignored] : harmony::chordQualities()) {
                (void)ignored;
                names.push_back(name);
            }
            add("unknown_chord", "Unknown chord quality '" + quality + "'.", at,
                "Did you mean '" + closest(quality, names) + "'?");
        } else if (code == "chord.bad_bass") {
            add("bad_pitch", "Invalid slash bass.", at, "Use a bass letter such as C/E.");
        } else {
            add("pitch_range", "Chord pitch exceeds MIDI range.", at, "Use a lower chord octave.");
        }
        return std::nullopt;
    }

    void compileVoice(Voice& voice) {
        if (voice.grid) {
            compileGrid(voice);
            return;
        }
        struct Tie {
            std::vector<int> pitches;
            std::vector<std::size_t> indices;
            std::int64_t end;
            Token at;
        };
        std::optional<Tie> tie;
        int running = voice.velocity;
        for (const auto& [bar, definition] : voice.bars) {
            const auto limit = bar == 0 ? result_.pickupTicks
                                        : internal::barTicks(meter(bar), result_.sequence.ppq);
            std::int64_t cursor = 0;
            for (const auto& row : definition.rows) {
                for (std::size_t i = 0; i < row.size(); ++i) {
                    const Token& token = row[i];
                    const auto& s = token.text;
                    if (auto it = internal::dynamics().find(s); it != internal::dynamics().end()) {
                        running = it->second;
                        continue;
                    }
                    if (s == "r:*") {
                        if (i + 1 < row.size())
                            add("fill_not_last", "Fill rest must end the bar.", token,
                                "Move r:* to the end.");
                        cursor = std::max(cursor, limit);
                        continue;
                    }
                    if (s.starts_with("r:")) {
                        Token dur{s.substr(2), token.line, token.column + 2};
                        if (auto n = duration(dur)) {
                            if (*n > std::numeric_limits<std::int64_t>::max() - cursor)
                                add("bad_duration", "Rest duration overflows tick time.", dur,
                                    "Use a shorter rest.");
                            else
                                cursor += *n;
                        }
                        continue;
                    }
                    const auto colon = s.find(':');
                    if (colon == std::string::npos) {
                        add("syntax", "Expected note:DUR or rest.", token,
                            "Use C4:q, [C4 E4]:h, or r:q.");
                        continue;
                    }
                    std::vector<int> pitches;
                    const auto head = std::string_view(s).substr(0, colon);
                    if (head.starts_with('[') && head.ends_with(']')) {
                        std::istringstream stream(std::string(head.substr(1, head.size() - 2)));
                        std::string name;
                        while (stream >> name) {
                            Token p{name, token.line, token.column + 1};
                            if (auto value = pitch(p)) pitches.push_back(*value);
                        }
                        auto sorted = pitches;
                        std::sort(sorted.begin(), sorted.end());
                        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
                            add("duplicate_pitch", "Chord repeats a pitch.", token,
                                "Remove the duplicate pitch.");
                    } else if (head.starts_with('{') && head.ends_with('}')) {
                        auto value = chord(head.substr(1, head.size() - 2), voice.octave, token);
                        if (value) pitches = std::move(*value);
                    } else {
                        Token p{std::string(head), token.line, token.column};
                        if (auto value = pitch(p)) pitches.push_back(*value);
                    }
                    std::string tail = s.substr(colon + 1);
                    const bool layered = !tail.empty() && tail.back() == '&';
                    if (layered) tail.pop_back();
                    const bool tied = !tail.empty() && tail.back() == '~';
                    if (tied) tail.pop_back();
                    bool staccato = false, accent = false;
                    while (!tail.empty() && (tail.back() == internal::kStaccato ||
                                             tail.back() == internal::kAccent)) {
                        if (tail.back() == internal::kStaccato)
                            staccato = true;
                        else
                            accent = true;
                        tail.pop_back();
                    }
                    const auto valueSep = tail.find(':');
                    Token dur{tail.substr(0, valueSep), token.line,
                              token.column + static_cast<int>(colon + 1)};
                    auto ticks = duration(dur, true);
                    if (!ticks) continue;
                    int vel = running, off = 0;
                    if (valueSep != std::string::npos) {
                        const auto rest = std::string_view(tail).substr(valueSep + 1);
                        const auto slash = rest.find('/');
                        Token vt{std::string(rest.substr(0, slash)), token.line,
                                 token.column + static_cast<int>(colon + valueSep + 2)};
                        if (auto n = velocity(vt)) vel = *n;
                        if (slash != std::string_view::npos) {
                            auto n = internal::integer(rest.substr(slash + 1));
                            if (!n || *n < 0 || *n > 127)
                                add("velocity_range", "Off velocity must be 0..127.", vt,
                                    "Use /0 through /127.");
                            else
                                off = static_cast<int>(*n);
                        }
                    }
                    if (accent) vel = std::min(127, vel + 20);
                    auto noteLength = staccato ? std::max<std::int64_t>(1, *ticks / 2) : *ticks;
                    const auto base = barStart(bar);
                    if (cursor > std::numeric_limits<std::int64_t>::max() - base ||
                        noteLength > std::numeric_limits<std::int64_t>::max() - base - cursor) {
                        add("bad_duration", "Note duration overflows tick time.", dur,
                            "Use a shorter duration.");
                        continue;
                    }
                    const auto start = base + cursor;
                    if (tie && tie->end == start && tie->pitches == pitches) {
                        for (auto index : tie->indices) {
                            if (noteLength > std::numeric_limits<std::int64_t>::max() -
                                                 result_.sequence.notes[index].length) {
                                add("bad_duration", "Tied note duration overflows tick time.", dur,
                                    "Use a shorter tied duration.");
                                continue;
                            }
                            result_.sequence.notes[index].length += noteLength;
                            result_.sequence.notes[index].offVelocity =
                                static_cast<std::uint8_t>(off);
                        }
                        if (tied)
                            tie->end += noteLength;
                        else
                            tie.reset();
                    } else {
                        if (tie) {
                            add(tie->end == start ? "tie_mismatch" : "dangling_tie",
                                "Tied note has no matching adjacent continuation.", tie->at,
                                "Continue the same pitch exactly at the note end.");
                            tie.reset();
                        }
                        Tie next{pitches, {}, start + noteLength, token};
                        for (int p : pitches) {
                            next.indices.push_back(result_.sequence.notes.size());
                            result_.sequence.notes.push_back(
                                {start, noteLength, static_cast<std::uint8_t>(voice.channel),
                                 static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(vel),
                                 static_cast<std::uint8_t>(off)});
                        }
                        if (tied && !pitches.empty()) tie = std::move(next);
                    }
                    if (!layered) {
                        if (*ticks > std::numeric_limits<std::int64_t>::max() - cursor)
                            add("bad_duration", "Bar cursor overflows tick time.", dur,
                                "Use a shorter duration.");
                        else
                            cursor += *ticks;
                    }
                }
            }
            if (cursor != limit) {
                Token at{"", definition.line, 1};
                add(bar == 0 ? "pickup_length" : "bar_length",
                    "Voice " + voice.name + " bar " + std::to_string(bar) + " has " +
                        std::to_string(cursor) + " ticks; needs " + std::to_string(limit) + ".",
                    at,
                    cursor < limit
                        ? "Add r:" + internal::durationName(limit - cursor, result_.sequence.ppq) +
                              " or end with r:*."
                        : "Remove or shorten events by " + std::to_string(cursor - limit) +
                              " ticks.");
            }
        }
        if (tie)
            add("dangling_tie", "Tie has no continuation.", tie->at,
                "Add the same pitch at the tie's end, or remove ~.");
    }

    void compileGrid(const Voice& voice) {
        for (const auto& [bar, definition] : voice.bars) {
            for (const auto& row : definition.rows) {
                if (row.size() < 2) continue;
                int pitchValue = -1;
                const auto& names = context_.drums && !context_.drums->names.empty()
                                        ? context_.drums->names
                                        : internal::gmDrums();
                for (const auto& [name, value] : names)
                    if (name == row[0].text) pitchValue = value;
                if (pitchValue < 0) {
                    auto n = internal::pitch(row[0].text);
                    if (n) pitchValue = *n;
                }
                if (pitchValue < 0) {
                    std::vector<std::string> options;
                    for (const auto& [name, ignored] : names) {
                        (void)ignored;
                        options.push_back(name);
                    }
                    add("unknown_drum", "Unknown drum '" + row[0].text + "'.", row[0],
                        "Did you mean '" + closest(row[0].text, options) + "'?");
                    continue;
                }
                std::string pattern;
                for (std::size_t i = 1; i < row.size(); ++i) pattern += row[i].text;
                const auto steps =
                    (bar == 0 ? result_.pickupTicks
                              : internal::barTicks(meter(bar), result_.sequence.ppq)) /
                    voice.step;
                if (voice.step <= 0 || static_cast<std::int64_t>(pattern.size()) != steps) {
                    add("grid_steps", "Grid row has " + std::to_string(pattern.size()) + " steps.",
                        row[1], "Use exactly " + std::to_string(steps) + " steps.");
                    continue;
                }
                for (std::size_t i = 0; i < pattern.size(); ++i) {
                    const char c = pattern[i];
                    int vel = 0;
                    if (c == 'x')
                        vel = voice.velocity;
                    else if (c == 'X')
                        vel = voice.accent;
                    else if (c == 'o')
                        vel = voice.ghost;
                    else if (c >= '1' && c <= '9')
                        vel = (static_cast<int>(c - '1') * 14) + 15;
                    else if (c != '.' && c != '-') {
                        add("grid_char", "Invalid grid character.", row[1],
                            "Use ., -, x, X, o, or 1..9.");
                        continue;
                    }
                    if (vel != 0) {
                        const auto base = barStart(bar);
                        const auto offset = static_cast<std::int64_t>(i) * voice.step;
                        if (offset > std::numeric_limits<std::int64_t>::max() - base) {
                            add("bad_duration", "Grid note start overflows tick time.", row[0],
                                "Use a shorter pickup.");
                            continue;
                        }
                        const auto start = base + offset;
                        if (voice.hitLength > std::numeric_limits<std::int64_t>::max() - start) {
                            add("bad_duration", "Grid note duration overflows tick time.", row[0],
                                "Use a shorter hit length.");
                            continue;
                        }
                        result_.sequence.notes.push_back({start, voice.hitLength,
                                                          static_cast<std::uint8_t>(voice.channel),
                                                          static_cast<std::uint8_t>(pitchValue),
                                                          static_cast<std::uint8_t>(vel), 0});
                    }
                }
            }
        }
    }

    void compileLane(const Lane& lane) {
        std::int64_t previousTick = -1;
        int previousValue = 0;
        bool havePrevious = false;
        for (const auto& point : lane.points) {
            const auto& t = point.tokens;
            if (t.size() < 2 || t.size() > 3) {
                add("syntax", "Lane point needs position and value.", t[0],
                    "Use 1.1 64 or 2.1 100 lin.");
                continue;
            }
            auto pos = position(t[0]);
            if (!pos) continue;
            const auto tick = pos->second;
            if (tick < previousTick)
                add("lane_order", "Lane position decreased.", t[0],
                    "Write points in non-decreasing time order.");
            double raw = 0;
            if (lane.kind == midi::ControlKind::PitchBend && t[1].text.ends_with("st")) {
                auto n =
                    internal::number(std::string_view(t[1].text).substr(0, t[1].text.size() - 2));
                if (n)
                    raw = std::clamp(std::round(*n / lane.range * 8192.0), -8192.0, 8191.0);
                else
                    raw = 100000;
            } else {
                auto n = internal::integer(t[1].text);
                raw = n ? static_cast<double>(*n) : 100000;
            }
            const int minimum = lane.kind == midi::ControlKind::PitchBend ? -8192 : 0;
            const int maximum = lane.kind == midi::ControlKind::PitchBend ? 8191 : 127;
            if (raw < minimum || raw > maximum) {
                add("lane_value", "Lane value is out of range.", t[1],
                    "Use " + std::to_string(minimum) + ".." + std::to_string(maximum) + ".");
                continue;
            }
            const int value = static_cast<int>(raw);
            const std::string shape = t.size() == 3 ? t[2].text : "step";
            if (shape != "step" && shape != "lin" && shape != "slow" && shape != "fast")
                add("syntax", "Unknown lane shape.", t.back(), "Use step, lin, slow, or fast.");
            auto emit = [&](std::int64_t when, int v) {
                result_.sequence.controls.push_back({when, lane.kind,
                                                     static_cast<std::uint8_t>(lane.channel),
                                                     static_cast<std::uint8_t>(lane.number), v});
            };
            if (havePrevious && tick > previousTick && shape != "step") {
                const auto interval = std::max(1, result_.sequence.ppq / 16);
                int lastValue = previousValue;
                // Check the remaining distance before advancing: a pickup
                // can put this segment within one interval of INT64_MAX.
                for (auto when = previousTick; tick - when > interval;) {
                    when += interval;
                    double phase = double(when - previousTick) / double(tick - previousTick);
                    if (shape == "slow")
                        phase *= phase;
                    else if (shape == "fast")
                        phase = 1 - (1 - phase) * (1 - phase);
                    const int v = static_cast<int>(
                        std::lround(previousValue + (value - previousValue) * phase));
                    if (v != lastValue) {
                        emit(when, v);
                        lastValue = v;
                    }
                }
                emit(tick, value);
            } else
                emit(tick, value);
            previousTick = tick;
            previousValue = value;
            havePrevious = true;
        }
    }

    void compileEvents(const Events& events) {
        for (const auto& point : events.points) {
            const auto& t = point.tokens;
            if (t.size() < 3) {
                add("syntax", "Event needs position and value.", t[0], "Use 1.1 program 5.");
                continue;
            }
            auto pos = position(t[0]);
            if (!pos) continue;
            if (t[1].text == "program") {
                auto n = internal::integer(t[2].text);
                if (t.size() != 3 || !n || *n < 0 || *n > 127)
                    add("lane_value", "Program must be 0..127.", t[2],
                        "Use program 0 through 127.");
                else
                    result_.sequence.controls.push_back(
                        {pos->second, midi::ControlKind::ProgramChange,
                         static_cast<std::uint8_t>(events.channel), 0, static_cast<int>(*n)});
            } else if (t[1].text == "sysex") {
                midi::Sysex sx;
                sx.tick = pos->second;
                bool valid = true;
                for (std::size_t i = 2; i < t.size(); ++i) {
                    unsigned int byte = 0;
                    auto [end, error] = std::from_chars(
                        t[i].text.data(), t[i].text.data() + t[i].text.size(), byte, 16);
                    if (error != std::errc{} || end != t[i].text.data() + t[i].text.size() ||
                        byte > 255) {
                        add("syntax", "Invalid sysex byte.", t[i],
                            "Use two-digit hexadecimal bytes.");
                        valid = false;
                    } else
                        sx.bytes.push_back(static_cast<std::uint8_t>(byte));
                }
                if (valid &&
                    (sx.bytes.size() < 2 || sx.bytes.front() != 0xf0 || sx.bytes.back() != 0xf7)) {
                    add("syntax", "Sysex must include F0 and F7.", t[2],
                        "Start with F0 and end with F7.");
                    valid = false;
                }
                if (valid) result_.sequence.sysex.push_back(std::move(sx));
            } else
                add("unknown_keyword", "Unknown event kind.", t[1], "Use program or sysex.");
        }
    }
};

}  // namespace

Result<Compiled> compile(std::string_view text, const CompileContext& context) {
    Compiler compiler(text, context);
    return compiler.run();
}

}  // namespace supple::notation
