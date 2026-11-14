// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Tempo map commands: tempo.set and timesig.* (session-model §7).
#include "command_helpers.h"

namespace supple::engine {

namespace {

// --- tempo ------------------------------------------------------------------------------

struct TempoPointArg {
    double beat = 0, bpm = 120;
    bool ramp = false;
};

struct TempoSetArgs {
    std::vector<TempoPointArg> points;
};

struct TimesigSetArgs {
    int bar = 1;
    std::string meter;
};

struct TimesigRemoveArgs {
    int bar = 2;
};

struct CountResult {
    std::int64_t count = 0;
};

}  // namespace

void registerTempoCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- tempo.set / timesig.set
    [&] {
        Command<TempoSetArgs, CountResult> c;
        c.name = "tempo.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Replace the tempo map: points {beat, bpm, ramp?}; the first must be at beat 0.";
        c.description =
            "ramp: true changes the tempo linearly (in beats) to the next point; otherwise it "
            "steps.";
        c.tags = {"tempo"};
        auto point = schema::object<TempoPointArg>("tempo_point")
                         .field("beat", &TempoPointArg::beat, schema::number().min(0))
                         .field("bpm", &TempoPointArg::bpm,
                                schema::number().range(1, 1000).unit(units::Unit::bpm))
                         .field("ramp", &TempoPointArg::ramp, schema::boolean())
                         .defaultValue(false)
                         .build();
        c.args = schema::object<TempoSetArgs>("tempo_set_args")
                     .field("points", &TempoSetArgs::points, schema::arrayOf(point))
                     .build();
        c.result = schema::object<CountResult>("count")
                       .field("count", &CountResult::count, schema::integer())
                       .build();
        c.examples = {
            {"Slow down after 8 bars",
             j(R"({"points": [{"beat": 0, "bpm": 120}, {"beat": 32, "bpm": 100, "ramp": false}]})"),
             j(R"({"count": 2})"), "empty"}};
        c.run = [](const TempoSetArgs& a, Context& ctx) -> Result<CountResult> {
            model::EditTxn& t = app(ctx).txn();
            if (a.points.empty() || a.points.front().beat != 0.0)
                return Error("tempo.first", "the first tempo point must be at beat 0",
                             "/points/0/beat", "set the first point beat to 0");
            for (std::size_t i = 1; i < a.points.size(); ++i)
                if (!(a.points[i].beat > a.points[i - 1].beat))
                    return Error(
                        "tempo.order", "tempo points must increase in beat",
                        "/points/" + std::to_string(i) + "/beat",
                        "sort points by increasing beat and remove duplicate beat positions");
            std::vector<Id> old;
            const auto current = app(ctx).view();
            for (const auto& e : current.tempo) old.push_back(e.id);
            for (Id id : old) t.remove<model::TempoPointObj>(id);
            for (const auto& p : a.points) t.create(model::TempoPointObj{p.beat, p.bpm, p.ramp});
            return CountResult{static_cast<std::int64_t>(a.points.size())};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TimesigSetArgs, Empty> c;
        c.name = "timesig.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set the meter from a bar on, e.g. {\"bar\": 9, \"meter\": \"3/4\"}.";
        c.tags = {"tempo"};
        c.args = schema::object<TimesigSetArgs>("timesig_set_args")
                     .field("bar", &TimesigSetArgs::bar, schema::integerOf<int>().min(1))
                     .field("meter", &TimesigSetArgs::meter,
                            schema::string().pattern("^[0-9]{1,2}/[0-9]{1,2}$", "N/D"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Waltz from bar 9", j(R"({"bar": 9, "meter": "3/4"})"), j("{}"), "empty"}};
        c.run = [](const TimesigSetArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = app(ctx).txn();
            const auto slash = a.meter.find('/');
            const int n = std::stoi(a.meter.substr(0, slash)),
                      d = std::stoi(a.meter.substr(slash + 1));
            if (n < 1 || n > 64 || d < 1 || d > 64 || (d & (d - 1)))
                return Error("timesig.invalid", "invalid meter " + a.meter, "/meter",
                             "N 1..64, D a power of 2");
            std::optional<Id> at;
            const auto current = app(ctx).view();
            for (const auto& e : current.meters)
                if (e.value->bar == a.bar) at = e.id;
            if (at)
                t.put(*at, model::MeterObj{a.bar, n, d});
            else
                t.create(model::MeterObj{a.bar, n, d});
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- timesig.remove
    [&] {
        Command<TimesigRemoveArgs, Empty> c;
        c.name = "timesig.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove the meter change at a bar: the meter before it continues.";
        c.description = "Bar 1's meter cannot be removed (change it with timesig.set).";
        c.tags = {"tempo"};
        c.args = schema::object<TimesigRemoveArgs>("timesig_remove_args")
                     .field("bar", &TimesigRemoveArgs::bar, schema::integerOf<int>().min(2))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Back to 4/4 at bar 9", j(R"({"bar": 9})"), j("{}"), "meter_change"}};
        c.run = [](const TimesigRemoveArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = app(ctx).txn();
            std::optional<Id> at;
            const auto current = app(ctx).view();
            for (const auto& e : current.meters)
                if (e.value->bar == a.bar) at = e.id;
            if (!at)
                return Error("timesig.unknown", "no meter change at bar " + std::to_string(a.bar),
                             "/bar", "timesig changes are listed by session.describe");
            t.remove<model::MeterObj>(*at);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
