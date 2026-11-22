// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Item commands: item.add/remove/move/set/duplicate/paste/split, midi.write and midi.get.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "clip_context.h"
#include "clip_follow.h"
#include "clip_launcher.h"
#include "command_helpers.h"
#include "heard_midi.h"
#include "supple/model/context.h"
#include "supple/model/follow.h"
#include "supple/model/launcher.h"

namespace supple::engine {

namespace {

// --- items / MIDI ------------------------------------------------------------------------

struct ItemAddArgs {
    std::string track;
    std::optional<double> position;  // beats
    std::optional<std::int64_t> bar;
    std::optional<double> length;  // beats
    std::optional<std::string> notation, audioFile, midiFile;
    std::optional<std::string> clip;          // a copy of a clip's takes (ADR-0097)
    std::optional<model::ClipFollow> follow;  // how the clip's notes follow the context
    std::optional<std::string> name;
};

struct ItemAddResult {
    std::string item, take;
    std::optional<std::string> source;
    double position = 0, length = 0;
    std::optional<std::vector<std::string>> warnings;
    std::optional<detail::FollowedInfo> followed;  // a clip's copy: what following did
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

struct MidiWriteResult {
    std::optional<std::string> follows;  // key | chords, when the written take follows
};

struct MidiGetArgs {
    std::string item;
    bool sounding = false;  // what sounds, not what is written (ADR-0098, following is playback)
    std::optional<double> from, to;  // song beats, with sounding: the heard notes of that range
};

// The key and the chords heard over a MIDI item's span, each from the bar of the item
// where it starts (item bars are the notation's; project bars are the arrange view's).
// A run of the key or the chords. On an item: the bar of the item and of the project where
// it starts. On a clip, which has no place in time: the clip's bar and its beat from the
// clip's start (ADR-0098).
struct ContextRun {
    std::int64_t itemBar = 1;
    std::optional<std::int64_t> projectBar;
    std::optional<double> beat;
    std::string what;
};
struct MidiContext {
    std::vector<ContextRun> keys, chords;
    std::optional<std::string> text;  // a clip's lead sheet in clip bars
};
struct MidiGetResult {
    std::string text;
    std::optional<MidiContext> context;
    std::optional<std::string> follow;       // a clip's or a following take's: none, key or chords
    std::optional<std::string> takeContext;  // a take's lead sheet, in the take's bars
    std::optional<std::int64_t> soundingDiffers;  // heard notes whose key differs from written
    std::optional<bool> differsByPass;  // sounding on a looped item: only its first pass answered
};

// The 1-based bar of the item that holds `itemBeat` (beats from the item's start).
std::int64_t itemBarOf(const std::function<midi::Meter(int)>& meterAt, double itemBeat) {
    double beat = 0;
    for (int bar = 1; bar < 1000000; ++bar) {
        const auto meter = meterAt(bar);
        beat += meter.numerator * 4.0 / meter.denominator;
        if (itemBeat < beat - 1e-9) return bar;
    }
    return 1000000;
}

std::optional<MidiContext> midiContext(const model::Session& s, const model::Item& item) {
    const double start = model::itemSpan(s, item).startBeat;
    const double end = start + model::itemLengthBeats(s, item);
    const auto meterAt = meterContext(s, start);
    auto run = [&](double from, bool holding, std::string what) {
        const double at = holding ? start : from;
        return ContextRun{itemBarOf(meterAt, at - start), s.timeMap->barBeat(at).bar, std::nullopt,
                          std::move(what)};
    };
    MidiContext out;
    // By beat; the one holding at the item's start leads, later ones fall inside.
    auto collect = [&](model::ContextType type, std::vector<ContextRun>& runs) {
        std::vector<std::pair<double, Id>> inside;
        for (const auto& e : s.context)
            if (e.value->type == type && e.value->beat > start && e.value->beat < end)
                inside.push_back({e.value->beat, e.id});
        std::sort(inside.begin(), inside.end());
        auto words = [&](const model::ContextPoint& p) {
            if (type == model::ContextType::Key) return model::keyWords(p);
            if (p.symbol.empty()) return std::string("(no chord)");
            return p.symbol + (model::parseChordPoint(p) ? "" : "?");
        };
        const auto holding = type == model::ContextType::Key ? model::keyPointAt(s, start)
                                                             : model::chordPointAt(s, start);
        if (holding)
            if (const auto* p = s.context.find(*holding))
                runs.push_back(run(start, true, words(*p)));
        for (const auto& [beat, id] : inside)
            runs.push_back(run(beat, false, words(*s.context.find(id))));
    };
    collect(model::ContextType::Key, out.keys);
    collect(model::ContextType::Chord, out.chords);
    if (out.keys.empty() && out.chords.empty()) return std::nullopt;
    return out;
}

// A clip's context as midi.get answers it (ADR-0098): the runs by clip bar and beat, and
// the lead sheet text. Nothing for a clip with no context.
std::optional<MidiContext> midiContext(const model::Session& s, const model::Clip& clip) {
    if (clip.context.empty()) return std::nullopt;
    const auto meterAt = meterContext(s, 0);
    MidiContext out;
    for (const model::ContextPoint& p : clip.context) {
        ContextRun run;
        run.itemBar = itemBarOf(meterAt, p.beat);
        run.beat = std::round(p.beat * 960.0) / 960.0;
        if (p.type == model::ContextType::Key) {
            run.what = model::keyWords(p);
            out.keys.push_back(std::move(run));
        } else {
            run.what =
                p.symbol.empty() ? "(no chord)" : p.symbol + (model::parseChordPoint(p) ? "" : "?");
            out.chords.push_back(std::move(run));
        }
    }
    out.text = detail::clipContextText(s, clip);
    return out;
}

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

// Search the same immutable source samples that playback reads. No decode,
// stretching or cache mutation belongs in this edit handler.
Result<double> priorZeroCrossing(EngineApp& owner, const model::Session& s, const model::Item& item,
                                 double cursor) {
    const auto* take = item.activeTake ? s.takes.find(*item.activeTake) : nullptr;
    if (!take || !take->source || take->source->kind != model::Kind::Media)
        return Error("item.split_not_audio", "zero-crossing split needs an active audio take",
                     "/item", "use snap none to split MIDI or empty items at the cursor");
    const auto span = model::itemSpan(s, item);
    if (!(cursor > span.startSec && cursor < span.endSec))
        return Error("item.split_range", "split must be inside the item", "/at",
                     "choose a beat strictly after the start and before the end");
    const auto& prepared = owner.media().sources();
    const Error pending("item.split_source_pending", "current source audio is not prepared",
                        "/item",
                        "start the engine (engine.start with backend dummy is sufficient), "
                        "wait for source preparation and retry");
    if (!prepared || prepared->session.project->mediaDir != s.project->mediaDir ||
        (owner.engine() && prepared->sampleRate != owner.engine()->config().sampleRate))
        return pending;
    const auto found = prepared->takes.find(*item.activeTake);
    if (found == prepared->takes.end()) return pending;
    const auto& source = found->second;
    // Per-take provenance lets a batch split several items from one snapshot.
    // Scalar variants do not depend on item placement or source offset.
    auto matches = [&](Id id) {
        const auto* now = s.takes.find(id);
        const auto* was = prepared->session.takes.find(id);
        if (!now || !was || now->source != was->source || now->playrate != was->playrate ||
            now->pitch != was->pitch || now->preservePitch != was->preservePitch ||
            now->pitchMode != was->pitchMode || now->stretchMarkers != was->stretchMarkers ||
            now->warpPeers != was->warpPeers)
            return false;
        const auto* media = now->source ? s.media.find(*now->source) : nullptr;
        const auto* oldMedia = was->source ? prepared->session.media.find(*was->source) : nullptr;
        return media && oldMedia && model::toJson(*media) == model::toJson(*oldMedia) &&
               (!source.itemLocal || now->startOffset == was->startOffset);
    };
    if (!matches(*item.activeTake)) return pending;
    if (source.itemLocal) {
        const model::Item* original = nullptr;
        for (const auto& entry : prepared->session.items)
            if (entry.value->activeTake == item.activeTake) original = entry.value.get();
        if (!original || item.loop != original->loop) return pending;
        const auto previous = model::itemSpan(prepared->session, *original);
        if (span.endSec - span.startSec != previous.endSec - previous.startSec) return pending;
        for (auto peer : take->warpPeers)
            if (!matches(peer)) return pending;
    } else if (model::activeWarp(*take) && !item.loop) {
        return pending;
    }
    if (source.error)
        return Error("item.split_source", source.error->message, "/item",
                     "relink or repair the audio source, then retry after source preparation");
    if (!source.data || !(prepared->sampleRate > 0)) return pending;
    const auto& audio = *source.data;
    const double rate = prepared->sampleRate;
    const auto frames = static_cast<std::int64_t>(audio.frames());
    if (audio.channels.empty() || frames == 0) return cursor;
    const MediaVariant variant{take->playrate, take->pitch, take->preservePitch, take->pitchMode};
    const auto offset =
        source.itemLocal ? 0 : time::samplesAt(take->startOffset, rate / variant.speed());
    const auto start = time::samplesAt(span.startSec, rate);
    const auto end = time::samplesAt(span.endSec, rate);
    const auto first =
        std::max(start + 1, static_cast<std::int64_t>(std::ceil((cursor - .1) * rate)));
    const auto last = std::min(end - 1, static_cast<std::int64_t>(std::floor(cursor * rate)));
    auto sample = [&](std::int64_t frame, std::size_t channel) {
        auto at = offset + frame - start;
        if (item.loop) at = ((at % frames) + frames) % frames;
        if (at < 0 || at >= frames) return 0.f;
        const auto n = static_cast<std::size_t>(at);
        float l = audio.channels[0][n];
        float r = audio.channels[audio.channelCount() > 1 ? 1 : 0][n];
        switch (take->channelMode) {
            case model::ChannelMode::Normal:
                break;
            case model::ChannelMode::ReverseStereo:
                std::swap(l, r);
                break;
            case model::ChannelMode::MonoMix:
                l = r = .5f * (l + r);
                break;
            case model::ChannelMode::Left:
                r = l;
                break;
            case model::ChannelMode::Right:
                l = r;
                break;
        }
        return channel == 0 ? l : r;
    };
    double energy[2]{};
    for (auto n = first; n <= last; ++n)
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double x = static_cast<double>(sample(n, ch));
            if (std::isfinite(x)) energy[ch] += x * x;
        }
    const std::size_t channel = energy[1] > energy[0] ? 1 : 0;
    // A fractional cursor in silence is already at zero; keep it exact.
    if (first <= last && sample(last, channel) == 0 && sample(last + 1, channel) == 0)
        return cursor;
    for (auto n = last; n >= first; --n) {
        const float x = sample(n, channel), previous = sample(n - 1, channel);
        const double at = static_cast<double>(n) / rate;
        if (at > span.startSec && at < span.endSec && at <= cursor && std::isfinite(x) &&
            std::isfinite(previous) &&
            (x == 0 || (previous < 0 && x > 0) || (previous > 0 && x < 0)))
            return at;
    }
    return cursor;
}

struct ItemSetArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> position, length, gainDb, fadeIn, fadeOut;
    std::optional<model::FadeShape> fadeInShape, fadeOutShape;
    std::optional<std::int64_t> bar;
    std::optional<bool> mute, loop, locked;
    std::optional<std::string> name, color;
    std::optional<model::ItemTimebase> timebase;
};

struct ItemDuplicateResult {
    std::string id;
    std::vector<std::string> linked;
};

struct ItemDuplicateArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> to;
};

struct PastePlacement {
    std::string item, track;
    double position = 0;
};

struct ItemPasteArgs {
    std::optional<std::int64_t> version;
    std::vector<PastePlacement> items;
};

struct ItemPasteResult {
    std::vector<std::string> items;
};

struct ItemSplitArgs {
    bool bypassGroups = false;
    std::string item;
    double at = 0;
    std::string snap = "none";
};

struct SplitResult {
    std::string left, right;
    std::vector<std::string> linkedLeft, linkedRight;
};

schema::Schema<model::FadeShape> fadeShapeSchema() {
    return schema::enumeration<model::FadeShape>({{model::FadeShape::Linear, "linear"},
                                                  {model::FadeShape::Slow, "slow"},
                                                  {model::FadeShape::Fast, "fast"},
                                                  {model::FadeShape::SlowFast, "slow_fast"},
                                                  {model::FadeShape::FastSlow, "fast_slow"},
                                                  {model::FadeShape::Cos, "cos"},
                                                  {model::FadeShape::Exp, "exp"}});
}

// item.add {clip}: a new item from a copy of the clip's takes, converted
// at the tempo where it lands (ADR-0097 "Converting between an item and a
// clip").
Result<ItemAddResult> addItemFromClip(Context& ctx, const model::Session& s, model::EditTxn& t,
                                      Id track, double pos, const std::string& text,
                                      std::optional<model::ClipFollow> follow) {
    auto id = existing<model::Clip>(s, text, "/clip");
    if (!id) return std::move(id).error();
    const model::Clip& clip = *s.clips.find(*id);
    if (clip.takes.empty() || !clip.activeTake)
        return Error("item.source", text + " has no take to lay on the timeline", "/clip",
                     "give the clip a take first (take.add), or remove it");
    SUPPLE_TRY(checkItemTrack(s, track, clip.takes, "/track"));
    const model::Take* active = s.takes.find(*clip.activeTake);
    if (!active)
        return Error("item.take", clip.activeTake->str() + " is missing", "/clip",
                     "use a current take id from take.list");
    auto shaped = itemFromClip(s, clip, *active, track, pos, "/clip");
    if (!shaped) return std::move(shaped).error();
    model::Item item = std::move(shaped->item);
    auto copied =
        copyTakesFor(t, s, clip.takes, false, shaped->envelopePos, "/clip", shaped->fxEnvelope);
    if (!copied) return std::move(copied).error();
    item.takes = *copied;
    for (std::size_t k = 0; k < clip.takes.size(); ++k)
        if (clip.activeTake == clip.takes[k]) item.activeTake = item.takes[k];
    nameItemFromClip(t, clip, item);
    const double length = model::itemLengthBeats(s, item);
    const Id it = t.create(std::move(item));
    // The laid takes carry the clip's context and follow: the notes sound as the key (or chords)
    // under the item says when asked to; otherwise as written, and context.mismatch says when the
    // clip's key is not the project's.
    std::vector<detail::ClipNote> notes;
    auto followed = detail::followPlacedItem(s, t, *id, clip, it, follow, "/clip", notes);
    if (!followed) return std::move(followed).error();
    for (const auto& n : notes) ctx.warn(n.code, n.message, n.path);
    ItemAddResult r;
    r.followed = std::move(*followed);
    r.item = it.str();
    r.take = t.preview().items.find(it)->activeTake->str();
    if (const model::Take* tk = t.preview().takes.find(*t.preview().items.find(it)->activeTake);
        tk && tk->source)
        r.source = tk->source->str();
    r.position = pos;
    r.length = length;
    return r;
}

}  // namespace

Result<AudioImport> importAudioFile(EngineApp& app, model::EditTxn& t, const model::Session& s,
                                    const std::string& path) {
    // The header, not a decode: the media model needs only the rate, the
    // channel count and the length, and adding one file to many items must
    // not decode it many times. The hash is the source's identity, so a
    // second add of the same bytes reuses the md_*.
    auto info = audio::inspect(path);
    if (!info) return std::move(info).error();
    auto hash = sha256File(path);
    if (!hash) return std::move(hash).error();
    AudioImport out;
    if (info->sampleRate > 0) out.seconds = static_cast<double>(info->frames) / info->sampleRate;
    // The same bytes are one media source: a second add of the same file
    // reuses the source it already has. A project saved before hashes
    // existed has a source with none, so the file's path and header decide,
    // and the source's hash is filled in with the bytes on disk now — an old
    // project converges rather than growing a second source for the same
    // file.
    std::optional<model::Id> known = model::mediaSourceWithHash(s, *hash);
    if (!known) {
        if (const auto legacy = legacyMediaSourceFor(s, path, *info, app.mediaDir()))
            known = *legacy;
        if (known)
            t.modify<model::MediaSource>(*known, [&](model::MediaSource& md) { md.hash = *hash; });
    }
    if (known) {
        out.source = *known;
        return out;
    }
    model::MediaSource md;
    md.path = fs::absolute(path).string();
    if (app.copyImportedMedia() && !app.projectDir().empty()) {
        // The project's own copy, so it never depends on the original staying
        // where it is (a replay finds the same bytes under the same name and
        // reuses them).
        bool created = false;
        auto name = detail::copyIntoFolder(path, app.mediaDir(), &created);
        if (!name) return std::move(name).error();
        if (created) app.noteCreatedFile(app.mediaDir() / *name);
        md.path = *name;
    }
    md.hash = *hash;
    md.format = formatName(info->container);
    md.sampleRate = info->sampleRate;
    md.channels = info->channels;
    md.frames = info->frames;
    out.source = t.create(std::move(md));
    return out;
}

Result<MidiFileImport> readMidiFile(EngineApp& app, const model::Session& s,
                                    const std::string& path, double atBeat) {
    auto bytes = readFile(app.resolve(path), midi::kMaxSmfBytes);
    if (!bytes) return std::move(bytes).error();
    midi::SmfInfo info;
    auto seq = midi::readSmf(*bytes, &info);
    if (!seq) return std::move(seq).error();
    MidiFileImport out;
    // The file's own end, else where its last event ends.
    std::int64_t end = seq->length;
    for (const auto& n : seq->notes) end = std::max(end, n.start + n.length);
    for (const auto& cc : seq->controls) end = std::max(end, cc.tick);
    seq->length = end;
    seq->canonicalize();
    if (end > 0) out.beats = static_cast<double>(end) / seq->ppq;
    const double bpm = s.timeMap->bpmAt(atBeat);
    for (const auto& [tick, tempo] : info.tempos)
        if (std::abs(tempo - bpm) > 1e-6) {
            out.warnings.push_back("the file's tempo (" + json::formatDouble(tempo) +
                                   " BPM) is not applied: its notes follow the project's tempo");
            break;
        }
    if (info.skippedEvents > 0)
        out.warnings.push_back(std::to_string(info.skippedEvents) +
                               " unsupported events in the file were skipped");
    out.source.sequence = std::move(*seq);
    return out;
}

void registerItemCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- item.add
    [&] {
        Command<ItemAddArgs, ItemAddResult> c;
        c.name = "item.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add an item on a track: notation, a MIDI or audio file, or a clip's copy.";
        c.description =
            "position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "
            "\"voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h\" — every bar must add up; the item "
            "is as long as the written bars unless length (beats) is given. audio_file: a path at "
            "the project's sample rate; with the app's copy-imports setting on, the project "
            "keeps its own copy in its media folder. midi_file: a Standard MIDI File (type 0 or 1, "
            "every track merged, channels kept); its notes keep their beats and follow the "
            "project's tempo (the file's tempo and meter are not applied; a warning says when they "
            "differ), and the item is as long as the file unless length is given. clip: a copy of "
            "that clip's takes, as long as the clip, its loop and take envelopes converted at "
            "the tempo where the item lands (ADR-0097); the item is named as the clip is (its "
            "active take's name) when the clip has a name. follow (none, key or chords; default: "
            "the clip's own) is set on the laid MIDI takes with the clip's context, so the notes "
            "sound as the key or chords under the item say, mapped from those they were written "
            "against (ADR-0098, following is playback): the notes stay as written, a context edit "
            "moves them, take.bake writes the sounding pitches, and notes on channel 10 and clips "
            "that cannot follow sound as written. The answer's followed says how they sound, by "
            "bar. With none context.mismatch warns when the clip's key, or in the same key its "
            "chord, is not the project's under the item. A master holds "
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
                .field("clip", &ItemAddArgs::clip,
                       schema::string()
                           .pattern("^cl_[0-9]+$", "clip id")
                           .doc("Lay a copy of this clip's takes here."))
                .field("follow", &ItemAddArgs::follow, detail::followArgSchema())
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
                .field("followed", &ItemAddResult::followed, detail::followedSchema())
                .build();
        c.errors = {
            {"notation.*", "the notation does not compile", "line:col and a hint in the error"},
            {"item.follow", "follow with a source other than clip",
             "follow applies to a clip's copy: give clip, or leave follow out"},
            {"tr.unknown", "no such track", "see track.list"}};
        c.examples = {
            {"A bass line at bar 1",
             j(R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q r:q C2:q r:q"})"),
             j(R"({"item": "it_1", "take": "tk_1", "source": "mi_1", "position": 0.0, "length": 4.0})"),
             "song"}};
        c.run = [](const ItemAddArgs& a, Context& ctx) -> Result<ItemAddResult> {
            EngineApp& app = engine::app(ctx);
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
            if ((a.notation ? 1 : 0) + (a.audioFile ? 1 : 0) + (a.midiFile ? 1 : 0) +
                    (a.clip ? 1 : 0) >
                1)
                return Error("item.source", "give one of notation, audio_file, midi_file or clip",
                             {}, "provide exactly one source for the item");
            if (a.follow && !a.clip)
                return Error("item.follow", "follow applies to a clip's copy", "/follow",
                             "give clip, or leave follow out");
            if (a.clip && a.length)
                return Error("item.source", "an item from a clip is as long as the clip", "/length",
                             "leave length out, or set it after with item.set");
            const double pos = beatsOf(
                s, a.position, a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
            SUPPLE_TRY(model::checkedBeat(s, pos, a.bar ? "/bar" : "/position"));
            if (a.length) SUPPLE_TRY(model::checkedBeat(s, pos + *a.length, "/length"));
            model::EditTxn& t = app.txn();
            model::Take take;
            take.name = a.name.value_or("");
            double length = a.length.value_or(4.0);
            ItemAddResult r;
            if (a.clip) return addItemFromClip(ctx, s, t, *track, pos, *a.clip, a.follow);
            if (a.notation) {
                auto compiled = compileNotation(s, *a.notation, pos);
                if (!compiled) return std::move(compiled).error();
                if (!a.length) length = compiled->second;
                const Id mi = t.create(std::move(compiled->first));
                take.source = mi;
                r.source = mi.str();
            } else if (a.midiFile) {
                auto file = readMidiFile(app, s, *a.midiFile, pos);
                if (!file) return std::move(file).error().at("/midi_file");
                for (auto& w : file->warnings) {
                    if (!r.warnings) r.warnings.emplace();
                    r.warnings->push_back(std::move(w));
                }
                if (!a.length && file->beats > 0) length = file->beats;
                const Id mi = t.create(std::move(file->source));
                take.source = mi;
                r.source = mi.str();
            } else if (a.audioFile) {
                auto imported = importAudioFile(app, t, s, *a.audioFile);
                if (!imported) return std::move(imported).error().at("/audio_file");
                if (!a.length && imported->seconds > 0)
                    length = s.timeMap->beatAt(s.timeMap->secondsAt(pos) + imported->seconds) - pos;
                take.source = imported->source;
                r.source = imported->source.str();
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
                     .field("item", &ItemRef::item, timelineItemArg())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"item": "it_1"})"), j("{}"), "song_with_item"}};
        c.run = [](const ItemRef& a, Context& ctx) -> Result<Empty> {
            SUPPLE_TRY(refuseClip(a.item, "/item", "remove a clip with clip.remove"));
            EngineApp& app = engine::app(ctx);
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
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Move an item in time (position in beats or bar) and/or to another track.";
        c.tags = {"item"};
        c.args = schema::object<ItemMoveArgs>("item_move_args")
                     .field("bypass_groups", &ItemMoveArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemMoveArgs::item, timelineItemArg())
                     .field("position", &ItemMoveArgs::position, schema::number().min(0))
                     .field("bar", &ItemMoveArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("track", &ItemMoveArgs::track, idArg("tr", "Track"))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Two bars later", j(R"({"item": "it_1", "bar": 3})"), j("{}"), "song_with_item"}};
        c.run = [](const ItemMoveArgs& a, Context& ctx) -> Result<Empty> {
            SUPPLE_TRY(refuseClip(a.item, "/item", "move a clip to another cell with clip.move"));
            EngineApp& app = engine::app(ctx);
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
        Command<MidiWriteArgs, MidiWriteResult> c;
        c.name = "midi.write";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Replace an item's or clip's MIDI with notation (keeping its place and length).";
        c.description =
            "The item or clip keeps its length: notation longer than it is stored but does not "
            "play past its end, and midi.beyond_length warns. Lengthen it first (item.set "
            "length, clip.set length), or write the notation to fit. The notes are stored as "
            "written: on a take that follows (take.set {follow}) its context and follow stay, "
            "so the new notes sound as the context says, and the answer's follows says key or "
            "chords. Chord symbols in the text do not change a take's context (take.set does).";
        c.tags = {"midi"};
        c.args = schema::object<MidiWriteArgs>("midi_write_args")
                     .field("item", &MidiWriteArgs::item, ownerIdArg())
                     .field("notation", &MidiWriteArgs::notation, schema::string())
                     .build();
        c.result = schema::object<MidiWriteResult>("midi_write_result")
                       .field("follows", &MidiWriteResult::follows, schema::string())
                       .build();
        c.examples = {{"Rewrite the bass line",
                       j(R"({"item": "it_1", "notation": "voice bass; 1: C2:e C2:e r:q G1:h"})"),
                       j("{}"), "song_with_item"}};
        c.run = [](const MidiWriteArgs& a, Context& ctx) -> Result<MidiWriteResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = existingOwner(s, a.item, "/item");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            const auto active = ownerActiveTake(s, *id);
            const model::Take* take = active ? s.takes.find(*active) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item",
                             "use item.add or clip.add with notation");
            auto compiled = compileNotationFull(s, a.notation, ownerStartBeat(s, *id));
            if (!compiled) return std::move(compiled).error();
            const model::MidiSource* old = s.midi.find(*take->source);
            compiled->source.pooled = old && old->pooled;
            model::EditTxn& txn = app.txn();
            const auto symbols = std::move(compiled->chords);
            const int ppq = compiled->source.sequence.ppq;
            txn.put<model::MidiSource>(*take->source, std::move(compiled->source));
            if (!old || !old->pooled)
                txn.modify<model::Take>(*active, [](model::Take& tk) {
                    // The new notes start at the item's start: its context beat stays what it
                    // was there (ADR-0098, the context is anchored to the source).
                    if (!tk.context.empty()) tk.contextPhase += tk.startOffset;
                    tk.startOffset = 0;
                });
            // A clip records the chord symbols the text holds as its chords (ADR-0098): they
            // replace the chords it had when they differ, and a text without symbols leaves
            // its context alone.
            if (const model::Clip* held = s.clips.find(*id); held && !symbols.empty()) {
                const double offset = old && old->pooled ? take->startOffset : 0.0;
                const double rate = take->playrate > 0 ? take->playrate : 1.0;
                auto made = detail::chordsFromSymbols(s, symbols, ppq, offset, rate, held->length);
                for (const auto& n : made.notes) ctx.warn(n.code, n.message, n.path);
                std::vector<model::ContextPoint> chords;
                for (const auto& p : held->context)
                    if (p.type == model::ContextType::Chord) chords.push_back(p);
                const auto grid = [](double beat) { return std::llround(beat * 960.0); };
                bool same = chords.size() == made.points.size();
                for (std::size_t k = 0; same && k < chords.size(); ++k)
                    same = grid(chords[k].beat) == grid(made.points[k].beat) &&
                           chords[k].symbol == made.points[k].symbol;
                if (!same && !made.points.empty()) {
                    model::Clip next = *held;
                    next.context.erase(std::remove_if(next.context.begin(), next.context.end(),
                                                      [](const model::ContextPoint& p) {
                                                          return p.type ==
                                                                 model::ContextType::Chord;
                                                      }),
                                       next.context.end());
                    for (auto& p : made.points) next.context.push_back(std::move(p));
                    model::fitClipContext(next);
                    txn.modify<model::Clip>(*id, [&](model::Clip& c2) { c2 = next; });
                    ctx.warn("clip.chords_set",
                             "the clip's chords are now the notation's " +
                                 std::to_string(made.points.size()) + " chord symbol" +
                                 (made.points.size() == 1 ? "" : "s") +
                                 (chords.empty()
                                      ? ""
                                      : " (replacing its " + std::to_string(chords.size()) + ")") +
                                 "; its keys are kept",
                             "/notation");
                }
            }
            // The owner keeps its length, so notation that runs past it is stored and
            // never plays: say so rather than let the caller think it did.
            const model::Clip* clip = s.clips.find(*id);
            const model::Item* item = clip ? nullptr : s.items.find(*id);
            const double room = clip ? clip->length : item ? model::itemLengthBeats(s, *item) : 0;
            const double rate = take->playrate > 0 ? take->playrate : 1.0;
            const auto& seq = txn.preview().midi.find(*take->source)->sequence;
            const double written = seq.ppq > 0 ? double(seq.length) / seq.ppq / rate : 0;
            if (room > 0 && written > room + 1e-6) {
                char buf[240];
                std::snprintf(buf, sizeof buf,
                              "the notation runs %.4g beats and %s is %.4g beats long: what lies "
                              "past its end is kept but does not play",
                              written, a.item.c_str(), room);
                ctx.warn("midi.beyond_length", buf, "/notation");
            }
            MidiWriteResult result;
            if (take->follow != model::ClipFollow::None)
                result.follows = take->follow == model::ClipFollow::Chords ? "chords" : "key";
            return result;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<MidiGetArgs, MidiGetResult> c;
        c.name = "midi.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read an item's or clip's MIDI as notation, with its key and chords.";
        c.description =
            "Pitch names follow the key and chord that hold at each note's start (a chord tone "
            "by its degree: G# over E, never Ab; none: the default spelling). context lists the "
            "key and the chords over the item's span, each from "
            "the bar of the item (item_bar, the notation's) and of the project (project_bar) "
            "where it starts; the first of each is the one already holding at the item's "
            "start. Omitted when the project has no key or chords there. A clip has no place "
            "in time: its pitch names follow its own key and chords, and its context is the "
            "key and chords it was written against: runs by clip bar (item_bar, no "
            "project_bar) and beat from its start, and text, its lead sheet in clip bars "
            "(clip.set {context} takes it back). follow (none, key, chords) is a clip's.";
        c.tags = {"midi"};
        c.args =
            schema::object<MidiGetArgs>("midi_get_args")
                .field("item", &MidiGetArgs::item, ownerIdArg())
                .field("sounding", &MidiGetArgs::sounding,
                       schema::boolean().doc("Answer what sounds (a following take's notes "
                                             "as they play), in item time, not what is "
                                             "written. A clip is refused: midi.sounding_clip."))
                .defaultValue(false)
                .field("from", &MidiGetArgs::from,
                       schema::number().doc("With sounding: the first song beat; the heard "
                                            "notes of from..to answer, passes as they fall."))
                .field("to", &MidiGetArgs::to,
                       schema::number().doc("With sounding: the end song beat."))
                .build();
        auto runSchema = schema::object<ContextRun>("context_run")
                             .field("item_bar", &ContextRun::itemBar, schema::integer())
                             .field("project_bar", &ContextRun::projectBar, schema::integer())
                             .field("beat", &ContextRun::beat, schema::number())
                             .field("what", &ContextRun::what, schema::string())
                             .build();
        c.result =
            schema::object<MidiGetResult>("midi_get")
                .field("text", &MidiGetResult::text, schema::string())
                .field("context", &MidiGetResult::context,
                       schema::object<MidiContext>("midi_context")
                           .field("keys", &MidiContext::keys, schema::arrayOf(runSchema))
                           .field("chords", &MidiContext::chords, schema::arrayOf(runSchema))
                           .field("text", &MidiContext::text, schema::string())
                           .build())
                .field("follow", &MidiGetResult::follow, schema::string())
                .field("take_context", &MidiGetResult::takeContext, schema::string())
                .field("sounding_differs", &MidiGetResult::soundingDiffers, schema::integer())
                .field("differs_by_pass", &MidiGetResult::differsByPass, schema::boolean())
                .build();
        c.errors = {{"midi.sounding_clip",
                     "a clip has no place in time, so what it sounds as is not defined",
                     "ask launch.state how it follows, or lay it on the timeline first"},
                    {"midi.range_args", "from and to go with sounding",
                     "give sounding: true, or leave from and to out"}};
        c.examples = {
            {"Read the bass line", j(R"({"item": "it_1"})"), j(R"({"text": "<any>"})"),
             "song_with_item"},
            {"Read what a following take sounds as", j(R"({"item": "it_1", "sounding": true})"),
             j(R"({"text": "<any>", "context": "<any>", "follow": "key",
                             "take_context": "1: key C major ~:w\n", "sounding_differs": 2})"),
             "song_following"}};
        c.run = [](const MidiGetArgs& a, Context& ctx) -> Result<MidiGetResult> {
            const model::Session s = app(ctx).view();
            auto id = existingOwner(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const auto active = ownerActiveTake(s, *id);
            const model::Take* take = active ? s.takes.find(*active) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item",
                             "choose an item with a MIDI take from track.list or create one with "
                             "item.add notation");
            notation::DecompileOptions options;
            const double start = ownerStartBeat(s, *id);
            options.meterAt = meterContext(s, start);
            const model::Clip* clip = s.clips.find(*id);
            if ((a.from || a.to) && !a.sounding)
                return Error("midi.range_args", "from and to go with sounding", "/from",
                             "give sounding: true, or leave from and to out");
            if (a.sounding && clip)
                return Error("midi.sounding_clip",
                             a.item +
                                 " is a clip: what it sounds as depends on where it is "
                                 "launched",
                             "/sounding",
                             "launch.state says how a launched clip follows; lay it on the "
                             "timeline (item.add {clip}) to read what sounds");
            // Names follow the key and chord that hold at each note's start (ADR-0100), as the
            // roll's do: a clip's own, never the project's; a following take's own when its
            // written notes are shown; otherwise the song's at the note's beat. Set below, once
            // the answer's notes are known.
            const midi::Sequence* shown = &s.midi.find(*take->source)->sequence;
            midi::Sequence heardSeq;  // what sounds, when asked (a following take only)
            MidiGetResult extra;      // the following-take fields
            // What was answered, in item time: the heard notes of a range, else (sounding) the
            // first pass of a looped item, else the item. `heardSeq` carries the controls placed
            // as the item plays them and each answered note at its place.
            const model::Item* itemObj = clip ? nullptr : s.items.find(*id);
            const auto answer = [&](const std::vector<model::SoundingNote>& notes, bool following,
                                    bool firstPass = false) {
                const midi::Sequence& written = *shown;
                const double itemStart = model::itemSpan(s, *itemObj).startBeat;
                heardSeq.ppq = written.ppq;
                heardSeq.length = std::llround(model::itemLengthBeats(s, *itemObj) * written.ppq);
                detail::heardMidi(s, *itemObj, *take, written, 0.0, heardSeq);
                heardSeq.notes.clear();
                // The first pass of a looped item: its controls and its length too, not every
                // pass's against one pass of notes.
                const double cycle = written.ppq > 0 ? static_cast<double>(written.length) /
                                                           static_cast<double>(written.ppq)
                                                     : 0.0;
                if (firstPass && itemObj->loop && cycle > 0) {
                    const double rate = take->playrate > 0 ? take->playrate : 1.0;
                    const double offset = std::fmod(take->startOffset, cycle);
                    const std::int64_t end =
                        std::llround((cycle - offset) / rate * static_cast<double>(written.ppq));
                    std::erase_if(heardSeq.controls,
                                  [&](const midi::Control& ctl) { return ctl.tick >= end; });
                    std::erase_if(heardSeq.sysex,
                                  [&](const midi::Sysex& x) { return x.tick >= end; });
                    heardSeq.length = std::min(heardSeq.length, end);
                }
                for (const auto& h : notes) {
                    midi::Note note = written.notes[h.note];
                    if (following && note.channel != 9)
                        note.pitch = static_cast<std::uint8_t>(h.sounds.pitch);
                    const double on = std::max(h.songBeat, itemStart) - itemStart;
                    note.start = std::llround(on * written.ppq);
                    note.length = std::max<std::int64_t>(
                        0, std::llround((h.endBeat - itemStart) * written.ppq) - note.start);
                    heardSeq.notes.push_back(note);
                }
                heardSeq.canonicalize();
                shown = &heardSeq;
            };
            const auto firstPass = [](std::vector<model::SoundingNote> notes) {
                notes.erase(std::remove_if(notes.begin(), notes.end(),
                                           [](const model::SoundingNote& h) { return h.pass > 0; }),
                            notes.end());
                return notes;
            };
            const auto differing = [](const std::vector<model::SoundingNote>& notes) {
                std::int64_t n = 0;
                for (const auto& h : notes)
                    if (h.channel != 9 && h.sounds.pitch != h.written) ++n;
                return n;
            };
            if (!clip && take->follow != model::ClipFollow::None) {
                const auto heard = model::soundingNotes(s, *id, -1e18, 1e18);
                const double itemStart = model::itemSpan(s, *itemObj).startBeat;
                extra.follow = take->follow == model::ClipFollow::Chords ? "chords" : "key";
                extra.takeContext =
                    detail::takeContextText(detail::takeBarMap(s, itemStart), *take);
                extra.soundingDiffers = differing(heard);
                if (a.sounding) {
                    if (a.from || a.to) {
                        // The heard notes of the range, each at its place in item time; the
                        // count is of those.
                        const auto range = model::soundingNotes(s, *id, a.from.value_or(-1e18),
                                                                a.to.value_or(1e18));
                        extra.soundingDiffers = differing(range);
                        answer(range, true);
                    } else {
                        // The first pass, in item time, as the item plays it; later passes that
                        // sound otherwise are said (`differs_by_pass`).
                        std::vector<std::int64_t> sounded;
                        for (const auto& h : heard)
                            sounded.push_back(h.channel == 9 ? h.written : h.sounds.pitch);
                        const model::LeadPitches lead =
                            model::leadPitches(heard, sounded, itemStart, shown->notes.size());
                        const auto first = firstPass(heard);
                        extra.soundingDiffers = differing(first);
                        answer(first, true, true);
                        if (!lead.alike) extra.differsByPass = true;
                    }
                }
            } else if (a.sounding && !clip) {
                // A take that does not follow sounds as written, in item time like the rest: the
                // heard notes of the range, or of the first pass.
                answer((a.from || a.to) ? model::soundingNotes(s, *id, a.from.value_or(-1e18),
                                                               a.to.value_or(1e18))
                                        : firstPass(model::soundingNotes(s, *id, -1e18, 1e18)),
                       false, !(a.from || a.to));
            }
            {
                const bool heard = shown == &heardSeq;
                const double ppq = shown->ppq > 0 ? static_cast<double>(shown->ppq) : 960.0;
                const bool own = !clip && !heard && take->follow != model::ClipFollow::None;
                // A source note sounds where `model::notePlace` puts it (the take's start offset
                // and playrate), and reads the take's context at its own context beat (phase and
                // period): the mapping the roll uses. A heard note is already in item time.
                options.placeAt = [&s, clip, own, heard, take, start, ppq](std::int64_t tick) {
                    const double beat = static_cast<double>(tick) / ppq;
                    if (heard) {
                        const double at = start + beat + model::kPlaceSlack;
                        return harmony::Place{model::keyAt(s, at), model::chordAt(s, at)};
                    }
                    const model::NotePlace where =
                        model::notePlace(*take, clip ? 0.0 : start, beat);
                    if (clip)
                        return harmony::Place{
                            model::keyAt(*clip, where.songBeat + model::kPlaceSlack),
                            model::chordAt(*clip, where.songBeat + model::kPlaceSlack)};
                    if (own) return model::takePlaceAt(take->context, where.contextBeat, true);
                    const double at = where.songBeat + model::kPlaceSlack;
                    return harmony::Place{model::keyAt(s, at), model::chordAt(s, at)};
                };
            }
            auto text = notation::decompile(*shown, options);
            if (!text) return std::move(text).error();
            if (clip) {
                const auto follow = clip->follow == model::ClipFollow::Key      ? "key"
                                    : clip->follow == model::ClipFollow::Chords ? "chords"
                                                                                : "none";
                extra.text = std::move(*text);
                extra.context = midiContext(s, *clip);
                extra.follow = follow;
                return extra;
            }
            const model::Item* item = s.items.find(*id);
            extra.text = std::move(*text);
            if (item) extra.context = midiContext(s, *item);
            return extra;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemSetArgs, Empty> c;
        c.name = "item.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set an item's position, length, playback, fades, name, color, or lock.";
        c.description =
            "Positions and lengths are in beats; fades are in seconds. Name is the active take's "
            "name. A locked item refuses every edit except {\"locked\": false}. timebase: what "
            "the item follows when the tempo changes — beats (moves and stretches with it), "
            "time (stays at its seconds), beats_position (its start follows the beat, its "
            "length stays in seconds) or track (inherit); the item keeps its place now.";
        c.tags = {"item"};
        c.args = schema::object<ItemSetArgs>("item_set_args")
                     .field("bypass_groups", &ItemSetArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemSetArgs::item, timelineItemArg())
                     .field("position", &ItemSetArgs::position, schema::number().min(0))
                     .field("bar", &ItemSetArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("length", &ItemSetArgs::length, schema::number().min(0))
                     .field("mute", &ItemSetArgs::mute, schema::boolean())
                     .field("loop", &ItemSetArgs::loop, schema::boolean())
                     .field("locked", &ItemSetArgs::locked,
                            schema::boolean().doc("The arrange view marks locked items."))
                     .field("gain_db", &ItemSetArgs::gainDb,
                            schema::number().range(model::kSilenceDb, 24).unit(units::Unit::dB))
                     .field("fade_in", &ItemSetArgs::fadeIn, schema::number().min(0))
                     .field("fade_in_shape", &ItemSetArgs::fadeInShape, fadeShapeSchema())
                     .field("fade_out_shape", &ItemSetArgs::fadeOutShape, fadeShapeSchema())
                     .field("fade_out", &ItemSetArgs::fadeOut, schema::number().min(0))
                     .field("name", &ItemSetArgs::name, schema::string())
                     .field("color", &ItemSetArgs::color, schema::string())
                     .field("timebase", &ItemSetArgs::timebase, model::itemTimebaseSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Lower an item", j(R"({"item":"it_1","gain_db":"-3 dB"})"), j("{}"), "song_with_item"},
            {"Keep an item at its time when the tempo changes",
             j(R"({"item":"it_1","timebase":"time"})"), j("{}"), "song_with_item"}};
        c.run = [](const ItemSetArgs& a, Context& ctx) -> Result<Empty> {
            SUPPLE_TRY(refuseClip(a.item, "/item", "set a clip's fields with clip.set"));
            const auto s = app(ctx).view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const auto& original = *s.items.find(*id);
            // A locked item refuses edits; unlocking it is the one exception.
            const bool unlocking = a.locked && !*a.locked;
            if (!unlocking) SUPPLE_TRY(checkUnlocked(s, *id));
            if (a.position && a.bar)
                return Error("item.position", "give position or bar, not both", "/position",
                             "remove one of them");
            if (a.length && !(*a.length > 0))
                return Error("item.length", "length must be greater than 0", "/length",
                             "give a positive length, or remove the item");
            model::Item it = original;
            if (a.timebase) {  // first: the rest is placed in the new domain
                const model::Timebase from = model::itemTimebase(s, it);
                it.timebase = *a.timebase;
                model::retimeItem(s, it, from, model::itemTimebase(s, it));
            }
            if (a.position || a.bar || a.length) {
                const double start = a.position ? *a.position
                                     : a.bar    ? s.timeMap->beatOfBar(static_cast<int>(*a.bar))
                                                : model::itemSpan(s, it).startBeat;
                if (a.length)
                    model::placeItem(s, it, start, *a.length);
                else
                    model::moveItem(s, it, it.track, start);
            }
            if (a.mute) it.mute = *a.mute;
            if (a.loop) it.loop = *a.loop;
            if (a.locked) it.locked = *a.locked;
            if (a.gainDb) it.gainDb = *a.gainDb;
            if (a.fadeIn) it.fadeIn.length = *a.fadeIn;
            if (a.fadeOut) it.fadeOut.length = *a.fadeOut;
            if (a.fadeInShape) it.fadeIn.shape = *a.fadeInShape;
            if (a.fadeOutShape) it.fadeOut.shape = *a.fadeOutShape;
            if (a.color) it.color = *a.color;
            if (a.name && !it.activeTake)
                return Error("item.no_take", "item has no active take to name", "/name",
                             "select an existing take with take.activate before setting name, or "
                             "add a take to the item");
            auto& t = app(ctx).txn();
            if (a.name)
                t.modify<model::Take>(*it.activeTake, [&](model::Take& tk) { tk.name = *a.name; });
            t.put<model::Item>(*id, std::move(it));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemDuplicateArgs, ItemDuplicateResult> c;
        c.name = "item.duplicate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Copy an item and its takes to a new position (default: right after).";
        c.tags = {"item"};
        c.args = schema::object<ItemDuplicateArgs>("item_duplicate_args")
                     .field("bypass_groups", &ItemDuplicateArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemDuplicateArgs::item, timelineItemArg())
                     .field("to", &ItemDuplicateArgs::to, schema::number().min(0))
                     .build();
        c.result =
            schema::object<ItemDuplicateResult>("item_duplicate_result")
                .field("id", &ItemDuplicateResult::id, schema::string())
                .field("linked", &ItemDuplicateResult::linked, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .build();
        c.examples = {{"Repeat", j(R"({"item":"it_1"})"), j(R"({"id":"it_2"})"), "song_with_item"}};
        c.run = [](const ItemDuplicateArgs& a, Context& ctx) -> Result<ItemDuplicateResult> {
            SUPPLE_TRY(refuseClip(
                a.item, "/item",
                "copy a clip with clip.duplicate, or lay it on the timeline with item.add {clip}"));
            const auto s = app(ctx).view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            model::Item copy = *s.items.find(*id);
            const double end = model::itemSpan(s, copy).startBeat + model::itemLengthBeats(s, copy);
            model::moveItem(s, copy, copy.track, a.to.value_or(end));
            copy.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldId : s.items.find(*id)->takes) {
                auto old = dangling<model::Take>(s, oldId, "/item");
                if (!old) return std::move(old).error();
                model::Take tk = **old;
                unpoolCopiedSource(tk, s, txn);
                auto cloned = copyTake(s, txn, oldId, std::move(tk));
                if (!cloned) return std::move(cloned).error();
                const Id fresh = *cloned;
                copy.takes.push_back(fresh);
                if (s.items.find(*id)->activeTake == oldId) copy.activeTake = fresh;
            }
            return ItemDuplicateResult{txn.create(std::move(copy)).str(), {}};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemPasteArgs, ItemPasteResult> c;
        c.name = "item.paste";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Copy items as they were in a version to tracks and beats.";
        c.description =
            "version: where the items are read (default: now) — a GUI copy or cut remembers the "
            "version, so a paste reproduces the items as copied even after they were removed or "
            "edited. items: [{item, track, position}] (beats; channels only). Takes, fades, "
            "take FX and MIDI are copied; an audio source the project no longer has is restored.";
        c.tags = {"item"};
        auto placement = schema::object<PastePlacement>("paste_placement")
                             .field("item", &PastePlacement::item, timelineItemArg())
                             .field("track", &PastePlacement::track, idArg("tr", "Track"))
                             .field("position", &PastePlacement::position, schema::number().min(0))
                             .build();
        c.args = schema::object<ItemPasteArgs>("item_paste_args")
                     .field("version", &ItemPasteArgs::version, schema::integer().min(1))
                     .field("items", &ItemPasteArgs::items, schema::arrayOf(placement))
                     .build();
        c.result = schema::object<ItemPasteResult>("item_paste_result")
                       .field("items", &ItemPasteResult::items, schema::arrayOf(schema::string()))
                       .build();
        c.examples = {{"Paste after itself",
                       j(R"({"items":[{"item":"it_1","track":"tr_1","position":16}]})"),
                       j(R"({"items":["it_2"]})"), "song_with_item"}};
        c.run = [](const ItemPasteArgs& a, Context& ctx) -> Result<ItemPasteResult> {
            for (std::size_t k = 0; k < a.items.size(); ++k)
                SUPPLE_TRY(refuseClip(a.items[k].item, "/items/" + std::to_string(k) + "/item",
                                      "copy clips with clip.duplicate, or lay one on the "
                                      "timeline with item.add {clip}"));
            EngineApp& eng = app(ctx);
            const auto now = eng.view();
            std::shared_ptr<const model::Session> from;
            if (a.version) {
                const model::Version* v =
                    eng.versions().version(static_cast<std::uint64_t>(*a.version));
                if (!v || !v->session)
                    return Error("history.version", "no version " + std::to_string(*a.version),
                                 "/version", "history.list shows the versions");
                from = v->session;
            } else {
                from = std::make_shared<const model::Session>(now);
            }
            const model::Session& s = *from;
            auto& txn = eng.txn();
            std::map<Id, std::vector<Id>> copiedTakes;
            std::map<Id, Id> restored;  // audio sources brought back, by their old id
            ItemPasteResult out;
            for (std::size_t i = 0; i < a.items.size(); ++i) {
                const PastePlacement& p = a.items[i];
                const std::string at = "/items/" + std::to_string(i);
                auto item = existing<model::Item>(s, p.item, at + "/item");
                if (!item) return std::move(item).error();
                auto track = existing<model::Track>(now, p.track, at + "/track");
                if (!track) return std::move(track).error();
                const model::Item& original = *s.items.find(*item);
                SUPPLE_TRY(checkItemTrack(now, *track, original.takes, at + "/track", &s));
                model::Item copy = original;
                // Its length as its timebase kept it in the copied version (beats
                // for beats, seconds otherwise), placed on today's tempo map.
                const model::ItemSpan span = model::itemSpan(s, original);
                const bool beats = model::itemTimebase(s, original) == model::Timebase::Beats;
                copy.track = *track;
                const double lengthBeats =
                    beats ? s.timeMap->beatAt(span.endSec) - span.startBeat
                          : now.timeMap->beatAt(now.timeMap->secondsAt(p.position) + span.endSec -
                                                span.startSec) -
                                p.position;
                model::placeItem(now, copy, p.position, lengthBeats);
                copy.takes.clear();
                copy.activeTake.reset();
                for (Id oldId : original.takes) {
                    auto old = dangling<model::Take>(s, oldId, at + "/item");
                    if (!old) return std::move(old).error();
                    model::Take tk = **old;
                    if (tk.source && tk.source->kind == model::Kind::Media &&
                        !now.media.find(*tk.source)) {
                        auto it = restored.find(*tk.source);
                        if (it == restored.end()) {
                            const model::MediaSource* md = s.media.find(*tk.source);
                            if (!md)
                                return Error("item.source", "the copied audio is gone", at,
                                             "recopy from a version containing the audio source, "
                                             "or reimport its audio_file");
                            it = restored.emplace(*tk.source, txn.create(*md)).first;
                        }
                        tk.source = it->second;
                    } else if (tk.source && tk.source->kind == model::Kind::Midi &&
                               !now.midi.find(*tk.source)) {
                        const model::MidiSource* mi = s.midi.find(*tk.source);
                        if (!mi)
                            return Error("item.source", "the copied MIDI is gone", at,
                                         "recopy from a version containing the MIDI source, or "
                                         "recreate the item with notation");
                        model::MidiSource fresh = *mi;
                        fresh.pooled = false;
                        tk.source = txn.create(std::move(fresh));
                    } else {
                        unpoolCopiedSource(tk, s, txn);
                    }
                    auto cloned = copyTake(s, txn, oldId, std::move(tk));
                    if (!cloned) return std::move(cloned).error();
                    const Id fresh = *cloned;
                    copiedTakes[oldId].push_back(fresh);
                    copy.takes.push_back(fresh);
                    if (original.activeTake == oldId) copy.activeTake = fresh;
                }
                out.items.push_back(txn.create(std::move(copy)).str());
            }
            for (const auto& [oldId, copies] : copiedTakes) {
                const auto& original = *s.takes.find(oldId);
                if (copies.size() != 1 || original.warpPeers.empty()) continue;
                std::vector<Id> peers;
                for (auto peer : original.warpPeers) {
                    const auto found = copiedTakes.find(peer);
                    if (found == copiedTakes.end() || found->second.size() != 1) {
                        peers.clear();
                        break;
                    }
                    peers.push_back(found->second.front());
                }
                if (peers.size() != original.warpPeers.size()) continue;
                std::sort(peers.begin(), peers.end());
                txn.modify<model::Take>(copies.front(),
                                        [&](auto& take) { take.warpPeers = peers; });
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemSplitArgs, SplitResult> c;
        c.name = "item.split";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Split an item at a beat, optionally at the prior audio zero crossing.";
        c.description =
            "snap defaults to none. zero_crossing searches the active take's prepared playback "
            "source up to 100 ms before at, before FX/fades/gain. After channel mode, use the "
            "louder stereo channel by window energy (left on ties), never a stereo sum unless "
            "MonoMix is selected. Choose the latest exact zero or sign-change sample boundary; "
            "never search forward or outside the item. Without a crossing, split at at. MIDI "
            "and empty takes refuse zero_crossing; the GUI skips them. Current prepared audio "
            "must be available; start the engine and retry after preparation. Linked items use "
            "the initiating item's resolved split time. History records that time for replay.";
        c.tags = {"item"};
        c.args = schema::object<ItemSplitArgs>("item_split_args")
                     .field("bypass_groups", &ItemSplitArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemSplitArgs::item, timelineItemArg())
                     .field("at", &ItemSplitArgs::at, schema::number().min(0))
                     .field("snap", &ItemSplitArgs::snap,
                            schema::enumeration<std::string>(
                                {{"none", "none"}, {"zero_crossing", "zero_crossing"}}))
                     .defaultValue(std::string("none"))
                     .build();
        c.result =
            schema::object<SplitResult>("item_split_result")
                .field("left", &SplitResult::left, schema::string())
                .field("right", &SplitResult::right, schema::string())
                .field("linked_left", &SplitResult::linkedLeft, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .field("linked_right", &SplitResult::linkedRight, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .build();
        c.errors = {{"item.split_range", "the cursor is outside the item's interior",
                     "choose a beat strictly after the start and before the end"},
                    {"item.split_not_audio", "zero_crossing needs an active audio take",
                     "use snap none to split MIDI or empty items at the cursor"},
                    {"item.split_source_pending", "current source audio is not prepared",
                     "start the engine, wait for source preparation and retry"},
                    {"item.split_source", "the audio source could not be prepared",
                     "relink or repair the audio source and retry"}};
        c.examples = {{"Split halfway", j(R"({"item":"it_1","at":2})"),
                       j(R"({"left":"it_1","right":"it_2"})"), "song_with_item"},
                      {"Split at a prior audio zero crossing",
                       j(R"({"item":"it_1","at":2,"snap":"zero_crossing"})"),
                       j(R"({"left":"it_1","right":"it_2"})"), "song_with_prepared_audio"}};
        c.resolve = [](ItemSplitArgs& a, Context& ctx) -> Status {
            if (a.snap != "zero_crossing") return {};
            const auto s = app(ctx).view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return id.error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            auto at =
                priorZeroCrossing(app(ctx), s, *s.items.find(*id), s.timeMap->secondsAt(a.at));
            if (!at) return at.error();
            a.at = s.timeMap->beatAt(*at);
            a.snap = "none";  // the bus logs the resolved beat for linked edits and replay
            return {};
        };
        c.run = [](const ItemSplitArgs& a, Context& ctx) -> Result<SplitResult> {
            SUPPLE_TRY(refuseClip(
                a.item, "/item",
                "a clip does not split; lay it on the timeline first (item.add {clip})"));
            const auto s = app(ctx).view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& old = *s.items.find(*id);
            SUPPLE_TRY(checkUnlocked(s, *id));
            const auto* tr = s.tracks.find(old.track);
            model::Timebase tb = s.project->timebase;
            if (tr) {
                if (tr->timebase == model::TrackTimebase::Time) tb = model::Timebase::Time;
                if (tr->timebase == model::TrackTimebase::Beats) tb = model::Timebase::Beats;
                if (tr->timebase == model::TrackTimebase::BeatsPosition)
                    tb = model::Timebase::BeatsPosition;
            }
            if (old.timebase == model::ItemTimebase::Time) tb = model::Timebase::Time;
            if (old.timebase == model::ItemTimebase::Beats) tb = model::Timebase::Beats;
            if (old.timebase == model::ItemTimebase::BeatsPosition)
                tb = model::Timebase::BeatsPosition;
            const double splitSec = s.timeMap->secondsAt(a.at);
            const double startBeat =
                tb == model::Timebase::Time ? s.timeMap->beatAt(old.position) : old.position;
            const double startSec = s.timeMap->secondsAt(startBeat);
            const double endSec = tb == model::Timebase::Time ? old.position + old.length
                                  : tb == model::Timebase::Beats
                                      ? s.timeMap->secondsAt(old.position + old.length)
                                      : startSec + old.length;
            if (!(splitSec > startSec && splitSec < endSec))
                return Error("item.split_range", "split must be inside the item", "/at",
                             "choose a beat strictly after the start and before the end");
            model::Item left = old, right = old;
            if (tb == model::Timebase::Time) {
                left.length = splitSec - startSec;
                right.position = splitSec;
                right.length = endSec - splitSec;
            } else if (tb == model::Timebase::Beats) {
                left.length = a.at - old.position;
                right.position = a.at;
                right.length = old.position + old.length - a.at;
            } else {
                left.length = splitSec - startSec;
                right.position = a.at;
                right.length = endSec - splitSec;
            }
            left.fadeOut.length = 0;
            right.fadeIn.length = 0;
            right.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldTake : old.takes) {
                auto source = dangling<model::Take>(s, oldTake, "/item");
                if (!source) return std::move(source).error();
                model::Take tk = **source;
                const double delta = tk.source && tk.source->kind == model::Kind::Midi
                                         ? (a.at - startBeat) * tk.playrate
                                         : (splitSec - startSec) * tk.playrate;
                if (tk.source && tk.source->kind == model::Kind::Media)
                    model::rebaseAudioTake(tk, splitSec - startSec);
                else
                    tk.startOffset += delta;
                unpoolCopiedSource(tk, s, txn);
                auto cloned = copyTake(s, txn, oldTake, std::move(tk), a.at - startBeat);
                if (!cloned) return std::move(cloned).error();
                const Id fresh = *cloned;
                right.takes.push_back(fresh);
                if (old.activeTake == oldTake) right.activeTake = fresh;
            }
            txn.put<model::Item>(*id, std::move(left));
            Id fresh = txn.create(std::move(right));
            return SplitResult{id->str(), fresh.str(), {}, {}};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
