// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/crash-resolver/resolve.h>
#include <supple/json/parse.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <system_error>

namespace supple::diagnostics::resolver {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t kMaxModules = 256, kMaxThreads = 128, kMaxFrames = 128;

Error bad(std::string pointer, const char* what) {
    return Error("report.invalid", std::string("the crash report is not valid: ") + what,
                 std::move(pointer),
                 "Pass an unmodified report written by this application "
                 "(schema supple.crash-report/1.0).");
}
std::optional<std::uint64_t> parseOffset(const std::string& text) {
    if (text.size() < 3 || text.size() > 18 || text[0] != '0' || text[1] != 'x')
        return std::nullopt;
    std::uint64_t value = 0;
    for (std::size_t i = 2; i < text.size(); ++i) {
        const char c = text[i];
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<std::uint64_t>(c - 'a' + 10);
        else
            return std::nullopt;
        value = (value << 4) | digit;
    }
    return value;
}
std::string hexOffset(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    do {
        text.insert(text.begin(), digits[value & 15]);
        value >>= 4;
    } while (value != 0);
    return "0x" + text;
}
bool lowerHex(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
const json::Value* member(const json::Value& object, const char* key) {
    return object.isObject() ? object.find(key) : nullptr;
}
bool oneOf(const json::Value* value, std::initializer_list<std::string_view> choices) {
    return value && value->isString() &&
           std::find(choices.begin(), choices.end(), value->asString()) != choices.end();
}

}  // namespace

Result<Report> parseReport(std::string_view text) {
    if (text.size() > kMaxReportBytes) return bad("", "it is larger than any report");
    json::ParseOptions options;
    options.maxBytes = kMaxReportBytes;
    options.maxDepth = 16;
    options.maxValues = 200000;
    auto parsed = json::parse(text, options);
    if (!parsed)
        return Error("report.invalid", "the file is not JSON: " + parsed.error().message, {},
                     "Pass a report file written by this application.");
    const json::Value& root = parsed.value();
    const auto* schema = member(root, "schema");
    if (!schema || !schema->isString() || schema->asString() != "supple.crash-report/1.0")
        return bad("/schema", "the schema is not supple.crash-report/1.0");
    Report report;
    for (const auto& [key, field, max] :
         {std::tuple<const char*, std::string*, std::size_t>{"incident", &report.incident, 32},
          {"role", &report.role, 16},
          {"platform", &report.platform, 16},
          {"kind", &report.kind, 32}}) {
        const auto* v = member(root, key);
        if (!v || !v->isString() || v->asString().size() > max)
            return bad(std::string("/") + key, "a field is missing or not a short string");
        *field = v->asString();
    }
    const auto* modules = member(root, "modules");
    const auto* threads = member(root, "threads");
    if (!modules || !modules->isArray() || modules->size() > kMaxModules)
        return bad("/modules", "modules is missing or too long");
    if (!threads || !threads->isArray() || threads->size() > kMaxThreads)
        return bad("/threads", "threads is missing or too long");
    for (std::size_t i = 0; i < modules->size(); ++i) {
        const auto& m = modules->at(i);
        const std::string at = "/modules/" + std::to_string(i);
        const auto* format = member(m, "format");
        const auto* id = member(m, "id");
        if (!oneOf(format, {"elf", "macho", "pe"})) return bad(at + "/format", "unknown format");
        ReportModule out;
        out.format = format->asString() == "elf"     ? Format::Elf
                     : format->asString() == "macho" ? Format::MachO
                                                     : Format::Pe;
        if (!id || (!id->isNull() && !id->isString())) return bad(at + "/id", "id is missing");
        if (id->isString()) {
            const auto& hex = id->asString();
            if (hex.size() < 2 || hex.size() > 128 || hex.size() % 2 != 0 || !lowerHex(hex))
                return bad(at + "/id", "id is not lowercase hexadecimal");
            out.id = hex;
        }
        report.modules.push_back(std::move(out));
    }
    for (std::size_t t = 0; t < threads->size(); ++t) {
        const auto& thread = threads->at(t);
        const std::string at = "/threads/" + std::to_string(t);
        const auto* role = member(thread, "role");
        const auto* state = member(thread, "state");
        const auto* frames = member(thread, "frames");
        const std::initializer_list<std::string_view> kRegistered{
            "model",         "audio_render", "audio_live", "audio_background",
            "audio_offline", "recorder",     "midi_output"};
        if (!oneOf(role, {"fault", "main", "model", "audio_render", "audio_live",
                          "audio_background", "audio_offline", "recorder", "midi_output", "other"}))
            return bad(at + "/role", "unknown role");
        const auto* registered = member(thread, "registered");
        if (registered && !oneOf(registered, kRegistered))
            return bad(at + "/registered", "unknown registered role");
        const auto* worker = member(thread, "worker");
        if (worker && (!worker->isInt() || worker->asInt() < 1 || worker->asInt() > 65535))
            return bad(at + "/worker", "worker is not a number from 1 to 65535");
        const auto* entryKind = member(thread, "entry");
        if (entryKind &&
            !oneOf(entryKind, {"create", "destroy", "activate", "deactivate", "start_processing",
                               "stop_processing", "process", "flush", "reset", "params", "state",
                               "semantic", "editor_picture", "trace", "gui", "main_thread",
                               "library_open", "library_close", "bridge_call"}))
            return bad(at + "/entry", "unknown entry kind");
        if (!oneOf(state, {"walked", "unavailable"})) return bad(at + "/state", "unknown state");
        if (!frames || !frames->isArray() || frames->size() > kMaxFrames)
            return bad(at + "/frames", "frames is missing or too long");
        ReportThread out;
        out.role = role->asString();
        if (registered) out.registered = registered->asString();
        if (entryKind) out.entry = entryKind->asString();
        if (worker) out.worker = static_cast<unsigned>(worker->asInt());
        out.state = state->asString();
        const auto* why = member(thread, out.state == "walked" ? "stop" : "reason");
        if (!why || !why->isString() || why->asString().size() > 32)
            return bad(at, "a thread has no stop or reason");
        out.why = why->asString();
        for (std::size_t f = 0; f < frames->size(); ++f) {
            const auto& frame = frames->at(f);
            const std::string fat = at + "/frames/" + std::to_string(f);
            const auto* module = member(frame, "module");
            const auto* ret = member(frame, "return");
            if (!module || !ret || !ret->isBool()) return bad(fat, "a frame is incomplete");
            ReportFrame outFrame;
            outFrame.isReturn = ret->asBool();
            if (!module->isNull()) {
                const auto* offset = member(frame, "offset");
                if (!module->isInt() || module->asInt() < 0 ||
                    static_cast<std::uint64_t>(module->asInt()) >= report.modules.size())
                    return bad(fat + "/module", "module is not an index into modules");
                if (!offset || !offset->isString())
                    return bad(fat + "/offset", "offset is missing");
                const auto value = parseOffset(offset->asString());
                if (!value) return bad(fat + "/offset", "offset is not 0x followed by hex digits");
                outFrame.module = static_cast<int>(module->asInt());
                outFrame.offset = *value;
            }
            out.frames.push_back(outFrame);
        }
        report.threads.push_back(std::move(out));
    }
    return report;
}

Result<Report> readReport(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return Error("report.unreadable", "the report is not a readable file", utf8(path),
                     "Pass the path of a .json file from the application's crash-report folder.");
    const auto size = fs::file_size(path, ec);
    if (ec || size > kMaxReportBytes)
        return Error("report.unreadable",
                     "the report is empty, unreadable or larger than any report", utf8(path),
                     "Pass an unmodified report written by this application.");
    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size())))
        return Error("report.unreadable", "the report could not be read", utf8(path));
    return parseReport(text);
}

namespace {
struct Walk {
    const ScanLimits& limits;
    const Limits& reader;
    ScanStats& stats;
    std::vector<Candidate>& out;
    bool full() { return stats.examined >= limits.maxFiles; }
    void file(const fs::path& path) {
        if (full()) {
            stats.truncated = true;
            return;
        }
        ++stats.examined;
        const FileSource source(path);
        if (!source.ok()) return;
        for (const auto& image : identify(source, reader)) out.push_back({path, image});
    }
    void directory(const fs::path& path, std::size_t depth) {
        std::error_code ec;
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            entries.push_back(it->path());
            if (entries.size() > limits.maxDirectoryEntries) {
                // Skipped whole: a partial listing would depend on the order the OS gave it.
                stats.truncated = true;
                return;
            }
        }
        std::sort(entries.begin(), entries.end());
        for (const auto& entry : entries) {
            if (full()) {
                stats.truncated = true;
                return;
            }
            std::error_code status;
            const auto type = fs::symlink_status(entry, status).type();
            if (status) continue;
            if (type == fs::file_type::regular)
                file(entry);
            else if (type == fs::file_type::directory) {
                if (depth < limits.maxDepth)
                    directory(entry, depth + 1);
                else
                    stats.truncated = true;
            }
        }
    }
};
}  // namespace

std::vector<Candidate> scan(const std::vector<fs::path>& roots, ScanStats& stats,
                            const ScanLimits& limits, const Limits& reader) {
    std::vector<Candidate> out;
    Walk walk{limits, reader, stats, out};
    for (const auto& root : roots) {
        std::error_code ec;
        if (fs::is_directory(root, ec))
            walk.directory(root, 0);
        else
            walk.file(root);
    }
    return out;
}

std::string_view stateName(ModuleState state) {
    switch (state) {
        case ModuleState::Resolved:
            return "resolved";
        case ModuleState::NoSymbols:
            return "no_symbols";
        case ModuleState::Unreadable:
            return "unreadable";
        case ModuleState::NoIdentity:
            return "no_identity";
        case ModuleState::NotFound:
            return "not_found";
    }
    return "not_found";
}

namespace {
// How many files with one id are read, at most, to find the one that has names.
constexpr std::size_t kMaxFilesPerId = 8;

// The frames of the report that are in a module with this format and id.
std::vector<ReportFrame> framesIn(const Report& report, Format format, const std::string& id) {
    std::vector<ReportFrame> frames;
    for (const auto& thread : report.threads)
        for (const auto& frame : thread.frames) {
            if (frame.module < 0 || static_cast<std::size_t>(frame.module) >= report.modules.size())
                continue;
            const auto& module = report.modules[static_cast<std::size_t>(frame.module)];
            if (module.format == format && module.id == id) frames.push_back(frame);
        }
    return frames;
}
// How many of `frames` the table names, counting a return address by the call before it.
std::size_t named(const SymbolTable& table, const std::vector<ReportFrame>& frames) {
    std::size_t count = 0;
    for (const auto& frame : frames) {
        const std::uint64_t back = frame.isReturn ? 1 : 0;
        if (frame.offset >= back && lookup(table, frame.offset - back)) ++count;
    }
    return count;
}
}  // namespace

Resolution resolve(const Report& report, const std::vector<Candidate>& candidates,
                   const Limits& limits) {
    Resolution out;
    // One answer per distinct (format, id): a report can name an image in every one of its
    // 256 module entries, and loading its symbol table that many times would take gigabytes.
    std::map<std::pair<Format, std::string>, ModuleResolution> answers;
    std::uint64_t held = 0;  // functions in the tables kept so far
    for (const auto& module : report.modules) {
        ModuleResolution r;
        if (module.id.empty()) {
            r.state = ModuleState::NoIdentity;
            out.modules.push_back(std::move(r));
            continue;
        }
        const auto key = std::make_pair(module.format, module.id);
        if (const auto known = answers.find(key); known != answers.end()) {
            out.modules.push_back(known->second);
            continue;
        }
        // Every file that carries the module's id is a candidate, not only the first: a
        // stripped binary and its separate debug file have the same build id, and the
        // stripped one, which still has a few exported names, usually comes first. The
        // file that names the most of this report's frames wins; the earlier one on a tie.
        const auto frames = framesIn(report, module.format, module.id);
        std::size_t tried = 0, best = 0;
        std::optional<ModuleResolution> chosen;
        ModuleResolution failed;  // the first file's reason, if none could be read
        const auto fail = [&](const Candidate& candidate, ModuleState state, std::string why) {
            if (failed.error.empty()) {
                failed.file = candidate.path;
                failed.state = state;
                failed.error = std::move(why);
            }
        };
        for (const auto& candidate : candidates) {
            if (candidate.image.format != module.format || candidate.image.id != module.id)
                continue;
            if (++tried > kMaxFilesPerId) break;
            const FileSource source(candidate.path);
            // The scan may be long past: a rebuild can have rewritten the file since. Its
            // identity is read again from the handle the symbols are read from, and must
            // still be the one the report names, at the same place.
            const auto same = [&] {
                for (const auto& image : identify(source, limits))
                    if (image.format == candidate.image.format && image.id == candidate.image.id &&
                        image.offset == candidate.image.offset &&
                        image.length == candidate.image.length)
                        return true;
                return false;
            };
            auto table = !source.ok() ? Result<SymbolTable>(Error("symbols.read_failed",
                                                                  "the file can no longer be read"))
                         : !same() ? Result<SymbolTable>(
                                         Error("symbols.changed",
                                               "the file changed after it was scanned, so it is no "
                                               "longer the build the report names"))
                                   : readSymbols(source, candidate.image, limits);
            if (!table) {
                fail(candidate,
                     table.error().code == "symbols.no_symbols" ? ModuleState::NoSymbols
                                                                : ModuleState::Unreadable,
                     table.error().message);
                continue;
            }
            if (held + table.value().symbols.size() > limits.maxTotalSymbols) {
                fail(candidate, ModuleState::Unreadable,
                     "the total of functions loaded for this report would pass " +
                         std::to_string(limits.maxTotalSymbols));
                continue;
            }
            const std::size_t score = named(table.value(), frames);
            if (!chosen || score > best) {
                best = score;
                chosen.emplace();
                chosen->file = candidate.path;
                chosen->table = std::make_shared<const SymbolTable>(std::move(table.value()));
            }
        }
        if (tried == 0) {
            r = ModuleResolution{};  // not found
        } else if (!chosen) {
            r = std::move(failed);
        } else {
            r = std::move(*chosen);
            if (frames.empty() || best > 0) {
                r.state = ModuleState::Resolved;
                held += r.table->symbols.size();
            } else {
                // A file with this id was read and has names, none of them for this report's
                // addresses (a stripped binary with no debug file beside it, typically).
                r.state = ModuleState::NoSymbols;
                r.error = "the file has " + std::to_string(r.table->symbols.size()) +
                          " functions but none of the report's frames in this module is in one; "
                          "a stripped binary's separate symbol file may be missing";
                r.table.reset();
            }
        }
        answers.emplace(key, r);
        out.modules.push_back(std::move(r));
    }
    return out;
}

std::optional<std::pair<std::string, std::uint64_t>> Resolution::locate(
    const ReportFrame& frame) const {
    if (frame.module < 0 || static_cast<std::size_t>(frame.module) >= modules.size())
        return std::nullopt;
    const auto& module = modules[static_cast<std::size_t>(frame.module)];
    if (!module.table) return std::nullopt;
    // A return address points after the call: the call is inside the function the byte before it
    // is.
    const std::uint64_t back = frame.isReturn ? 1 : 0;
    if (frame.offset < back) return std::nullopt;
    const auto match = lookup(*module.table, frame.offset - back);
    if (!match) return std::nullopt;
    return std::make_pair(demangle(match->name), match->displacement + back);
}
std::optional<std::string> Resolution::name(const ReportFrame& frame) const {
    auto found = locate(frame);
    if (!found) return std::nullopt;
    return std::move(found->first);
}

namespace {
// The length of the well-formed UTF-8 sequence that starts at text[i] (1 to 4), 0 if the
// bytes there are not one: a lone continuation byte, an overlong form, a surrogate, a value
// beyond U+10FFFF, a truncated sequence or a byte that is never valid.
std::size_t utf8Length(std::string_view text, std::size_t i) {
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(text[i + k]); };
    const unsigned char lead = at(0);
    if (lead < 0x80) return 1;
    std::size_t length = 0;
    unsigned char low = 0x80, high = 0xbf;  // the range the first continuation byte may take
    if (lead >= 0xc2 && lead <= 0xdf) {
        length = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        length = 3;
        if (lead == 0xe0) low = 0xa0;   // no overlong forms
        if (lead == 0xed) high = 0x9f;  // no surrogates
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        length = 4;
        if (lead == 0xf0) low = 0x90;   // no overlong forms
        if (lead == 0xf4) high = 0x8f;  // nothing beyond U+10FFFF
    } else {
        return 0;
    }
    if (i + length > text.size()) return 0;
    if (at(1) < low || at(1) > high) return 0;
    for (std::size_t k = 2; k < length; ++k)
        if (at(k) < 0x80 || at(k) > 0xbf) return 0;
    return length;
}

// Text for a terminal: a control byte, DEL, a C1 control (U+0080 to U+009F, two bytes in
// UTF-8, which some terminals obey) or any byte that is not part of well-formed UTF-8
// (a terminal in a Latin-1 locale obeys a raw 0x9b as CSI) becomes \xNN, so a hostile
// symbol file, report or file name cannot set a window title, clear the screen or write
// the clipboard. JSON output has its own escaping.
std::string printable(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    const auto escape = [&](unsigned char c) {
        out += "\\x";
        out += kHex[c >> 4];
        out += kHex[c & 15];
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7f) {
            escape(c);
            ++i;
            continue;
        }
        const std::size_t length = c < 0x80 ? 1 : utf8Length(text, i);
        const bool c1 = length == 2 && c == 0xc2 && static_cast<unsigned char>(text[i + 1]) <= 0x9f;
        if (length == 0 || c1) {
            for (std::size_t k = 0; k < (length ? length : 1); ++k)
                escape(static_cast<unsigned char>(text[i + k]));
            i += length ? length : 1;
        } else {
            out.append(text.substr(i, length));
            i += length;
        }
    }
    return out;
}
}  // namespace

std::string formatText(const Report& report, const Resolution& resolution) {
    std::ostringstream out;
    out << "incident " << printable(report.incident) << ": " << printable(report.kind) << " in "
        << printable(report.role) << " (" << printable(report.platform) << ")\nmodules:\n";
    const auto label = [&](std::size_t i) {
        const auto& r = resolution.modules[i];
        return r.file.empty() ? "module " + std::to_string(i) : printable(utf8(r.file.filename()));
    };
    for (std::size_t i = 0; i < report.modules.size(); ++i) {
        const auto& m = report.modules[i];
        const auto& r = resolution.modules[i];
        out << "  " << i << "  " << formatName(m.format) << "  "
            << (m.id.empty() ? "-" : printable(m.id)) << "  " << stateName(r.state);
        if (!r.file.empty()) out << "  " << printable(utf8(r.file));
        if (r.table)
            out << "  (" << r.table->symbols.size() << " functions"
                << (r.table->truncated ? ", truncated" : "") << ")";
        if (!r.error.empty()) out << "  (" << printable(r.error) << ")";
        out << "\n";
    }
    for (std::size_t t = 0; t < report.threads.size(); ++t) {
        const auto& thread = report.threads[t];
        out << "thread " << t << " " << printable(thread.role);
        if (!thread.registered.empty()) out << " (" << printable(thread.registered) << ")";
        if (!thread.entry.empty()) out << " in " << printable(thread.entry);
        out << ": " << printable(thread.state) << " ("
            << (thread.state == "walked" ? "stop: " : "reason: ") << printable(thread.why) << ")\n";
        for (std::size_t f = 0; f < thread.frames.size(); ++f) {
            const auto& frame = thread.frames[f];
            out << "  #" << f << "  ";
            if (frame.module < 0) {
                out << "(generated code)\n";
                continue;
            }
            out << label(static_cast<std::size_t>(frame.module)) << "+" << hexOffset(frame.offset);
            if (const auto found = resolution.locate(frame)) {
                out << "  " << printable(found->first);
                if (found->second != 0) out << " + " << hexOffset(found->second);
            } else {
                out << "  (no symbol)";
            }
            out << "\n";
        }
    }
    return out.str();
}

json::Value formatJson(const Report& report, const Resolution& resolution) {
    auto root = json::Value::object();
    root["schema"] = "supple.crash-symbols/1.0";
    root["incident"] = report.incident;
    root["kind"] = report.kind;
    root["role"] = report.role;
    root["platform"] = report.platform;
    json::Array modules;
    for (std::size_t i = 0; i < report.modules.size(); ++i) {
        const auto& m = report.modules[i];
        const auto& r = resolution.modules[i];
        auto entry = json::Value::object();
        entry["format"] = formatName(m.format);
        entry["id"] = m.id.empty() ? json::Value(nullptr) : json::Value(m.id);
        entry["state"] = stateName(r.state);
        if (!r.file.empty()) entry["file"] = utf8(r.file);
        if (r.table) {
            entry["functions"] = static_cast<std::uint64_t>(r.table->symbols.size());
            entry["truncated"] = r.table->truncated;
        }
        if (!r.error.empty()) entry["error"] = r.error;
        modules.push_back(std::move(entry));
    }
    root["modules"] = std::move(modules);
    json::Array threads;
    for (std::size_t t = 0; t < report.threads.size(); ++t) {
        const auto& thread = report.threads[t];
        auto entry = json::Value::object();
        entry["ordinal"] = static_cast<std::uint64_t>(t);
        entry["role"] = thread.role;
        if (!thread.registered.empty()) entry["registered"] = thread.registered;
        if (thread.worker != 0) entry["worker"] = thread.worker;
        if (!thread.entry.empty()) entry["entry"] = thread.entry;
        entry["state"] = thread.state;
        entry[thread.state == "walked" ? "stop" : "reason"] = thread.why;
        json::Array frames;
        for (const auto& frame : thread.frames) {
            auto f = json::Value::object();
            f["module"] = frame.module < 0 ? json::Value(nullptr) : json::Value(frame.module);
            if (frame.module >= 0) f["offset"] = hexOffset(frame.offset);
            f["return"] = frame.isReturn;
            if (const auto found = resolution.locate(frame)) {
                f["function"] = found->first;
                f["displacement"] = found->second;
            }
            frames.push_back(std::move(f));
        }
        entry["frames"] = std::move(frames);
        threads.push_back(std::move(entry));
    }
    root["threads"] = std::move(threads);
    return root;
}

}  // namespace supple::diagnostics::resolver
