// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// audition.play, audition.stop, audition.get (ADR-0094): hear a file on the
// device's outputs 1/2 before adding it.
#include "command_helpers.h"

namespace supple::engine {

namespace {

struct AuditionArgs {
    std::string path;
};

schema::Schema<AuditionStatus> statusSchema() {
    return schema::object<AuditionStatus>("audition_status")
        .field("playing", &AuditionStatus::playing, schema::boolean())
        .field("decoding", &AuditionStatus::decoding, schema::boolean())
        .field("path", &AuditionStatus::path, schema::string())
        .field("seconds", &AuditionStatus::seconds, schema::number())
        .field("position", &AuditionStatus::position, schema::number())
        .build();
}

json::Value silentStatus() {
    return j(R"({"playing": false, "decoding": false, "path": "", "seconds": 0.0,
                 "position": 0.0})");
}

}  // namespace

void registerAuditionCommands(cmd::Registry& registry) {
    [&] {
        Command<AuditionArgs, AuditionInfo> c;
        c.name = "audition.play";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Play an audio file once on outputs 1/2, outside the project.";
        c.description =
            "A WAV, FLAC or MP3 file plays from its start, once, on the device's outputs 1 and "
            "2 (mono on both; a wider file's first two channels), whether the transport is "
            "stopped or playing. It does not pass the project's tracks, masters or meters, and "
            "renders and recordings never contain it. The file's start, up to 28,800,000 frames "
            "at the higher of its rate and the device's (ten minutes at 48 kHz, 2.5 at 192 "
            "kHz), is read and resampled to the device's rate before it sounds (decoding: true "
            "meanwhile; truncated: true when the file is longer). path: absolute, or a name in the "
            "project's media folder. A second call replaces what plays; it ends at the file's "
            "end, with audition.stop, when the engine stops or another project is opened. Not "
            "an undo step, not saved.";
        c.tags = {"transport", "media"};
        c.args = schema::object<AuditionArgs>("audition_args")
                     .field("path", &AuditionArgs::path, schema::string())
                     .build();
        c.result = schema::object<AuditionInfo>("audition")
                       .field("path", &AuditionInfo::path, schema::string())
                       .field("seconds", &AuditionInfo::seconds, schema::number())
                       .field("sample_rate", &AuditionInfo::sampleRate, schema::number())
                       .field("channels", &AuditionInfo::channels, schema::integerOf<int>())
                       .field("truncated", &AuditionInfo::truncated, schema::boolean())
                       .build();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"},
                    {"audition.file", "the file is missing, unreadable or not WAV, FLAC or MP3",
                     "give a WAV, FLAC or MP3 file: an absolute path, or a name in the project's "
                     "media folder"}};
        c.examples = {{"A file in the project's media folder", j(R"({"path": "tone.wav"})"),
                       j(R"({"path": "<any>", "seconds": 2.0, "sample_rate": 48000.0,
                               "channels": 2, "truncated": false})"),
                       "audition_file"}};
        c.run = [](const AuditionArgs& a, Context& ctx) -> Result<AuditionInfo> {
            return app(ctx).playAudition(a.path);
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, AuditionStatus> c;
        c.name = "audition.stop";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Stop the file audition (fading out over 5 ms); returns its state.";
        c.description = "Stopping when nothing plays is not an error. Not an undo step.";
        c.tags = {"transport", "media"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = statusSchema();
        c.examples = {{"Nothing playing", j("{}"), silentStatus(), "engine_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<AuditionStatus> {
            EngineApp& a = app(ctx);
            a.stopAudition();
            return a.auditionStatus();
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, AuditionStatus> c;
        c.name = "audition.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Whether an audition plays: path, length, position (seconds).";
        c.tags = {"transport", "media"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = statusSchema();
        c.examples = {{"Nothing playing", j("{}"), silentStatus(), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<AuditionStatus> {
            return app(ctx).auditionStatus();
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
