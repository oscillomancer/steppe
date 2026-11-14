// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Item commands: item.add, item.remove, item.move, midi.write and midi.get (session-model §7).
#include "command_helpers.h"

namespace supple::engine {

namespace {

// --- items / MIDI ------------------------------------------------------------------------

struct ItemAddArgs {
    std::string track;
    std::optional<double> position;  // beats
    std::optional<std::int64_t> bar;
    std::optional<double> length;  // beats
    std::optional<std::string> notation, audioFile, midiFile;
    std::optional<std::string> name;
};

struct ItemAddResult {
    std::string item, take;
    std::optional<std::string> source;
    double position = 0, length = 0;
    std::optional<std::vector<std::string>> warnings;
};

struct ItemRef {
    bool bypassGroups = false;
    std::string item;
};

struct ItemMoveArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> position;
    std::optional<std::int64_t> bar;
    std::optional<std::string> track;
};

struct MidiWriteArgs {
    std::string item;
    std::string notation;
};

struct MidiGetArgs {
    std::string item;
};

// The model's name for a container ("wav"/"flac"/"mp3"): the detected one, not
// the file's extension.
const char* formatName(audio::Container container) {
    switch (container) {
        case audio::Container::Wav:
            return "wav";
        case audio::Container::Flac:
            return "flac";
        case audio::Container::Mp3:
            return "mp3";
        case audio::Container::Unknown:
            return "";
    }
    return "";
}

// A media source added before hashes existed: there is no hash to match, so the
// path (resolved the way the loader resolves it) and the header it was added
// from decide whether the file is still the one it came from. Reusing it lets a
// pre-hash project converge on one source per file instead of duplicating it.
std::optional<model::Id> legacyMediaSourceFor(const model::Session& s, const fs::path& file,
                                              const audio::FileInfo& info, const fs::path& media) {
    const auto same = [](const fs::path& a, const fs::path& b) {
        std::error_code ea, eb;
        const fs::path ca = fs::weakly_canonical(a, ea);
        const fs::path cb = fs::weakly_canonical(b, eb);
        return (ea ? a.lexically_normal() : ca) == (eb ? b.lexically_normal() : cb);
    };
    for (const auto& e : s.media) {
        const model::MediaSource& md = *e.value;
        if (!md.hash.empty()) continue;  // a hashed source is matched by its bytes
        if (md.sampleRate != info.sampleRate || md.channels != info.channels ||
            md.frames != info.frames)
            continue;  // not the recording the source was added from
        std::filesystem::path path = md.path;
        if (path.is_relative()) {
            const fs::path inMedia = media / path;
            path = fs::exists(inMedia) ? inMedia : media.parent_path() / path;
        }
        if (same(path, file)) return e.id;
    }
    return std::nullopt;
}

}  // namespace

void registerItemCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- item.add
    [&] {
        Command<ItemAddArgs, ItemAddResult> c;
        c.name = "item.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add an item on a track: MIDI in notation, a MIDI file or an audio file.";
        c.description =
            "position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "
            "\"voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h\" — every bar must add up; the item "
            "is as long as the written bars unless length (beats) is given. audio_file: a path at "
            "the project's sample rate; with the app's copy-imports setting on, the project "
            "keeps its own copy in its media folder. midi_file: a Standard MIDI File (type 0 or 1, "
            "every track merged, channels kept); its notes keep their beats and follow the "
            "project's tempo (the file's tempo and meter are not applied; a warning says when they "
            "differ), and the item is as long as the file unless length is given. A master holds "
            "audio items only (prints, played after its fader); busses hold none.";
        c.tags = {"item", "midi"};
        c.args =
            schema::object<ItemAddArgs>("item_add_args")
                .field("track", &ItemAddArgs::track, idArg("tr", "Track"))
                .field("position", &ItemAddArgs::position, schema::number().min(0).doc("Beats."))
                .field("bar", &ItemAddArgs::bar,
                       schema::integer()
                           .range(1, std::numeric_limits<int>::max())
                           .doc("Start at this bar (1-based)."))
                .field("length", &ItemAddArgs::length,
                       schema::number().exclusiveMin(0).doc("Beats."))
                .field("notation", &ItemAddArgs::notation, schema::string())
                .field("audio_file", &ItemAddArgs::audioFile, schema::string())
                .field("midi_file", &ItemAddArgs::midiFile, schema::string())
                .field("name", &ItemAddArgs::name, schema::string())
                .build();
        c.result =
            schema::object<ItemAddResult>("item_add")
                .field("item", &ItemAddResult::item, schema::string())
                .field("take", &ItemAddResult::take, schema::string())
                .field("source", &ItemAddResult::source, schema::string())
                .field("position", &ItemAddResult::position, schema::number())
                .field("length", &ItemAddResult::length, schema::number())
                .field("warnings", &ItemAddResult::warnings, schema::arrayOf(schema::string()))
                .build();
        c.errors = {
            {"notation.*", "the notation does not compile", "line:col and a hint in the error"},
            {"tr.unknown", "no such track", "see track.list"}};
        c.examples = {
            {"A bass line at bar 1",
             obj(R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q r:q C2:q r:q"})"),
             obj(R"({"item": "it_1", "take": "tk_1", "source": "mi_1", "position": 0.0, "length": 4.0})"),
             "song"}};
        c.run = [](const ItemAddArgs& a, Context& ctx) -> Result<ItemAddResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto track = existing<model::Track>(s, a.track, "/track");
            if (!track) return std::move(track).error();
            SUPPLE_TRY(checkItemTrack(s, *track, {}, "/track"));
            if ((a.notation || a.midiFile) &&
                s.tracks.find(*track)->type == model::StripType::Master)
                return Error("track.strip_type", "a master holds audio items only",
                             a.notation ? "/notation" : "/midi_file",
                             "give audio_file for a master, or put the MIDI on a channel "
                             "with an instrument");
            if ((a.notation ? 1 : 0) + (a.audioFile ? 1 : 0) + (a.midiFile ? 1 : 0) > 1)
                return Error("item.source", "give one of notation, audio_file or midi_file", {},
                             "provide exactly one source for the item");
            const double pos = beatsOf(
                s, a.position, a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
            SUPPLE_TRY(model::checkedBeat(s, pos, a.bar ? "/bar" : "/position"));
            if (a.length) SUPPLE_TRY(model::checkedBeat(s, pos + *a.length, "/length"));
            model::EditTxn& t = app.txn();
            model::Take take;
            take.name = a.name.value_or("");
            double length = a.length.value_or(4.0);
            ItemAddResult r;
            if (a.notation) {
                auto compiled = compileNotation(s, *a.notation, pos);
                if (!compiled) return std::move(compiled).error();
                if (!a.length) length = compiled->second;
                const Id mi = t.create(std::move(compiled->first));
                take.source = mi;
                r.source = mi.str();
            } else if (a.midiFile) {
                auto bytes = readFile(app.resolve(*a.midiFile), midi::kMaxSmfBytes);
                if (!bytes) return std::move(bytes).error().at("/midi_file");
                midi::SmfInfo info;
                auto seq = midi::readSmf(*bytes, &info);
                if (!seq) return std::move(seq).error().at("/midi_file");
                auto warn = [&r](std::string w) {
                    if (!r.warnings) r.warnings.emplace();
                    r.warnings->push_back(std::move(w));
                };
                // The file's own end, else where its last event ends.
                std::int64_t end = seq->length;
                for (const auto& n : seq->notes) end = std::max(end, n.start + n.length);
                for (const auto& cc : seq->controls) end = std::max(end, cc.tick);
                seq->length = end;
                seq->canonicalize();
                if (!a.length && end > 0) length = static_cast<double>(end) / seq->ppq;
                const double bpm = s.timeMap->bpmAt(pos);
                for (const auto& [tick, tempo] : info.tempos)
                    if (std::abs(tempo - bpm) > 1e-6) {
                        warn("the file's tempo (" + json::formatDouble(tempo) +
                             " BPM) is not applied: its notes follow the "
                             "project's tempo");
                        break;
                    }
                if (info.skippedEvents > 0)
                    warn(std::to_string(info.skippedEvents) +
                         " unsupported events in the file were skipped");
                model::MidiSource src;
                src.sequence = std::move(*seq);
                const Id mi = t.create(std::move(src));
                take.source = mi;
                r.source = mi.str();
            } else if (a.audioFile) {
                // The header, not a decode: the media model needs only the rate,
                // the channel count and the length, and adding one file to many
                // items must not decode it many times. The hash is the source's
                // identity, so a second add of the same bytes reuses the md_*.
                auto info = audio::inspect(*a.audioFile);
                if (!info) return std::move(info).error().at("/audio_file");
                auto hash = sha256File(*a.audioFile);
                if (!hash) return std::move(hash).error().at("/audio_file");
                if (!a.length && info->sampleRate > 0) {
                    const double secs = static_cast<double>(info->frames) / info->sampleRate;
                    length = s.timeMap->beatAt(s.timeMap->secondsAt(pos) + secs) - pos;
                }
                // The same bytes are one media source: a second add of the same
                // file reuses the source it already has. A project saved before
                // hashes existed has a source with none, so the file's path and
                // header decide, and the source's hash is filled in with the
                // bytes on disk now — an old project converges rather than
                // growing a second source for the same file.
                std::optional<model::Id> known = model::mediaSourceWithHash(s, *hash);
                if (!known) {
                    if (const auto legacy =
                            legacyMediaSourceFor(s, *a.audioFile, *info, app.mediaDir()))
                        known = *legacy;
                    if (known)
                        t.modify<model::MediaSource>(
                            *known, [&](model::MediaSource& md) { md.hash = *hash; });
                }
                if (known) {
                    take.source = *known;
                    r.source = known->str();
                } else {
                    model::MediaSource md;
                    md.path = fs::absolute(*a.audioFile).string();
                    if (app.copyImportedMedia() && !app.projectDir().empty()) {
                        // The project's own copy, so it never depends on the
                        // original staying where it is (a replay finds the
                        // same bytes under the same name and reuses them).
                        auto name = detail::copyIntoFolder(*a.audioFile, app.mediaDir());
                        if (!name) return std::move(name).error().at("/audio_file");
                        md.path = *name;
                    }
                    md.hash = *hash;
                    md.format = formatName(info->container);
                    md.sampleRate = info->sampleRate;
                    md.channels = info->channels;
                    md.frames = info->frames;
                    const Id md_ = t.create(std::move(md));
                    take.source = md_;
                    r.source = md_.str();
                }
            }
            const Id tk = t.create(std::move(take));
            model::Item item;
            item.track = *track;
            model::placeItem(s, item, pos, length);  // in the track's timebase
            item.takes = {tk};
            item.activeTake = tk;
            const Id it = t.create(std::move(item));
            r.item = it.str();
            r.take = tk.str();
            r.position = pos;
            r.length = length;
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- item.remove / item.move
    [&] {
        Command<ItemRef, Empty> c;
        c.name = "item.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove an item with its takes (unpooled sources go too).";
        c.tags = {"item"};
        c.args = schema::object<ItemRef>("item_ref")
                     .field("bypass_groups", &ItemRef::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemRef::item, idArg("it", "Item"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"item": "it_1"})"), obj("{}"), "song_with_item"}};
        c.run = [](const ItemRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            model::EditTxn& t = app.txn();
            for (Id tk : s.items.find(*id)->takes) SUPPLE_TRY(removeTakeObjects(t, s, tk));
            t.remove<model::Item>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemMoveArgs, Empty> c;
        c.name = "item.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move an item in time (position in beats or bar) and/or to another track.";
        c.tags = {"item"};
        c.args = schema::object<ItemMoveArgs>("item_move_args")
                     .field("bypass_groups", &ItemMoveArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemMoveArgs::item, idArg("it", "Item"))
                     .field("position", &ItemMoveArgs::position, schema::number().min(0))
                     .field("bar", &ItemMoveArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("track", &ItemMoveArgs::track, idArg("tr", "Track"))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Two bars later", obj(R"({"item": "it_1", "bar": 3})"), obj("{}"), "song_with_item"}};
        c.run = [](const ItemMoveArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            std::optional<Id> track;
            if (a.track) {
                auto tr = existing<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                SUPPLE_TRY(checkItemTrack(s, *tr, s.items.find(*id)->takes, "/track"));
                track = *tr;
            }
            app.txn().modify<model::Item>(*id, [&](model::Item& it) {
                const double start =
                    a.position || a.bar
                        ? beatsOf(
                              s, a.position,
                              a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt)
                        : model::itemSpan(s, it).startBeat;
                model::moveItem(s, it, track.value_or(it.track), start);
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- midi.write / midi.get
    [&] {
        Command<MidiWriteArgs, Empty> c;
        c.name = "midi.write";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Replace an item's MIDI with notation (the item keeps its position and length).";
        c.tags = {"midi"};
        c.args = schema::object<MidiWriteArgs>("midi_write_args")
                     .field("item", &MidiWriteArgs::item, idArg("it", "Item"))
                     .field("notation", &MidiWriteArgs::notation, schema::string())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Rewrite the bass line",
                       obj(R"({"item": "it_1", "notation": "voice bass; 1: C2:e C2:e r:q G1:h"})"),
                       obj("{}"), "song_with_item"}};
        c.run = [](const MidiWriteArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            const model::Item& it = *s.items.find(*id);
            const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item",
                             "use item.add with notation");
            auto compiled = compileNotation(s, a.notation, model::itemSpan(s, it).startBeat);
            if (!compiled) return std::move(compiled).error();
            const model::MidiSource* old = s.midi.find(*take->source);
            compiled->first.pooled = old && old->pooled;
            model::EditTxn& txn = app.txn();
            txn.put<model::MidiSource>(*take->source, std::move(compiled->first));
            if (!old || !old->pooled)
                txn.modify<model::Take>(*it.activeTake,
                                        [](model::Take& tk) { tk.startOffset = 0; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<MidiGetArgs, TextResult> c;
        c.name = "midi.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read an item's MIDI as notation.";
        c.tags = {"midi"};
        c.args = schema::object<MidiGetArgs>("midi_get_args")
                     .field("item", &MidiGetArgs::item, idArg("it", "Item"))
                     .build();
        c.result = schema::object<TextResult>("text")
                       .field("text", &TextResult::text, schema::string())
                       .build();
        c.examples = {{"Read the bass line", obj(R"({"item": "it_1"})"),
                       obj(R"({"text": "<any>"})"), "song_with_item"}};
        c.run = [](const MidiGetArgs& a, Context& ctx) -> Result<TextResult> {
            const model::Session s = A(ctx).view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& it = *s.items.find(*id);
            const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item",
                             "choose an item with a MIDI take from track.list or create one with "
                             "item.add notation");
            notation::DecompileOptions options;
            options.meterAt = meterContext(s, model::itemSpan(s, it).startBeat);
            auto text = notation::decompile(s.midi.find(*take->source)->sequence, options);
            if (!text) return std::move(text).error();
            return TextResult{std::move(*text)};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
