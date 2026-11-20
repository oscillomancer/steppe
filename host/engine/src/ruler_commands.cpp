// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Ruler commands: markers, regions and time.convert.
#include "command_helpers.h"

namespace supple::engine {

namespace {

struct RulerArgs {
    std::optional<double> pos, start, end;
    std::optional<std::string> name, color;
    std::optional<bool> section;  // regions only
};

struct RulerSetArgs {
    std::string id;
    std::optional<double> pos, start, end;
    std::optional<std::string> name, color;
    std::optional<bool> section;  // regions only
};

struct RulerRef {
    std::string id;
};

struct RulerEntry {
    model::Timebase timebase = model::Timebase::Beats;
    std::string id, name, color;
    std::optional<double> pos, start, end;
    std::optional<bool> section;  // regions only
};

struct RulerList {
    std::vector<RulerEntry> entries;
};

struct TimeArgs {
    std::optional<double> seconds, beat;
    std::optional<std::int64_t> bar;
};

struct TimeResult {
    double seconds = 0, beat = 0;
    std::int64_t bar = 1, beatInBar = 1;
    double fraction = 0;
};

template <class T, bool Region, bool WithId>
schema::Schema<T> rulerArgsSchema(const char* name) {
    auto b = schema::object<T>(name);
    if constexpr (WithId)
        b.field("id", &T::id, idArg(Region ? "rg" : "mk", Region ? "Region" : "Marker"));
    if constexpr (Region) {
        b.field("start", &T::start, schema::number().min(0));
        b.field("end", &T::end, schema::number().min(0));
    } else {
        b.field("pos", &T::pos, schema::number().min(0));
    }
    b.field("name", &T::name, schema::string());
    b.field("color", &T::color, schema::string());
    if constexpr (Region)
        b.field("section", &T::section,
                schema::boolean().doc("Whether the region is a section of the song, which "
                                      "context.at and session.describe report [true]"));
    return b.build();
}

auto rulerListSchema(bool region) {
    auto entry = schema::object<RulerEntry>("ruler_entry")
                     .field("id", &RulerEntry::id, schema::string())
                     .field("timebase", &RulerEntry::timebase, model::timebaseSchema())
                     .field("name", &RulerEntry::name, schema::string())
                     .field("color", &RulerEntry::color, schema::string())
                     .field("pos", &RulerEntry::pos, schema::number())
                     .field("start", &RulerEntry::start, schema::number())
                     .field("end", &RulerEntry::end, schema::number())
                     .field("section", &RulerEntry::section, schema::boolean())
                     .build();
    return schema::object<RulerList>("ruler_list")
        .field(region ? "regions" : "markers", &RulerList::entries, schema::arrayOf(entry))
        .build();
}

}  // namespace

void registerRulerCommands(cmd::Registry& registry) {
    auto rulerCommands = [&]<bool Region>() {
        using Object = std::conditional_t<Region, model::Region, model::Marker>;
        const std::string stem = Region ? "region" : "marker";
        const std::string fixture = Region ? "song_with_region" : "song_with_marker";
        const std::string exampleId = Region ? "rg_1" : "mk_1";
        {
            Command<RulerArgs, IdResult> c;
            c.name = stem + ".add";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = Region ? "Add a named region in beats." : "Add a named marker in beats.";
            c.tags = {"ruler"};
            c.args = rulerArgsSchema<RulerArgs, Region, false>((stem + "_add_args").c_str());
            c.result = idResultSchema();
            c.examples = {{"Add",
                           Region ? j(R"({"start":0,"end":4,"name":"Verse"})")
                                  : j(R"({"pos":4,"name":"Verse"})"),
                           j(std::string("{\"id\":\"") + exampleId + "\"}"), "empty"}};
            c.run = [](const RulerArgs& a, Context& ctx) -> Result<IdResult> {
                Object o;
                if constexpr (Region) {
                    if (!a.start || !a.end || *a.end <= *a.start)
                        return Error("region.range", "end must follow start", "/end",
                                     "give start and end in beats");
                    o.start = *a.start;
                    o.end = *a.end;
                    if (a.section) o.section = *a.section;
                } else {
                    if (!a.pos)
                        return Error("marker.pos", "marker position is required", "/pos",
                                     "provide pos as a nonnegative position in beats");
                    o.pos = *a.pos;
                }
                if (a.name) o.name = *a.name;
                if (a.color) o.color = *a.color;
                return IdResult{app(ctx).txn().create(std::move(o)).str()};
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<RulerSetArgs, Empty> c;
            c.name = stem + ".set";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = "Edit a ruler's position, name, or color.";
            c.tags = {"ruler"};
            c.args = rulerArgsSchema<RulerSetArgs, Region, true>((stem + "_set_args").c_str());
            c.result = emptySchema();
            c.examples = {{"Rename",
                           j(std::string("{\"id\":\"") + exampleId + "\",\"name\":\"Bridge\"}"),
                           j("{}"), fixture}};
            c.run = [](const RulerSetArgs& a, Context& ctx) -> Result<Empty> {
                const auto s = app(ctx).view();
                auto id = existing<Object>(s, a.id, "/id");
                if (!id) return std::move(id).error();
                Object o = *model::Traits<Object>::map(s).find(*id);
                if constexpr (Region) {
                    if (a.start) o.start = *a.start;
                    if (a.end) o.end = *a.end;
                    if (a.section) o.section = *a.section;
                    if (!(o.end > o.start))
                        return Error("region.range", "end must follow start", "/end",
                                     "set end later than start in beats");
                } else {
                    if (a.pos) o.pos = *a.pos;
                }
                if (a.name) o.name = *a.name;
                if (a.color) o.color = *a.color;
                app(ctx).txn().put<Object>(*id, std::move(o));
                return Empty{};
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<RulerRef, Empty> c;
            c.name = stem + ".remove";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = "Remove a ruler.";
            c.tags = {"ruler"};
            c.args = schema::object<RulerRef>("ruler_ref")
                         .field("id", &RulerRef::id,
                                idArg(Region ? "rg" : "mk", Region ? "Region" : "Marker"))
                         .build();
            c.result = emptySchema();
            c.examples = {
                {"Remove", j(std::string("{\"id\":\"") + exampleId + "\"}"), j("{}"), fixture}};
            c.run = [](const RulerRef& a, Context& ctx) -> Result<Empty> {
                auto id = existing<Object>(app(ctx).view(), a.id, "/id");
                if (!id) return std::move(id).error();
                app(ctx).txn().remove<Object>(*id);
                return Empty{};
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<cmd::NoArgs, RulerList> c;
            c.name = stem + ".list";
            c.kind = Kind::Query;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = "List rulers in this project.";
            c.tags = {"ruler"};
            c.args = schema::object<cmd::NoArgs>("no_args").build();
            c.result = rulerListSchema(Region);
            c.examples = {{"List empty", j("{}"),
                           Region ? j(R"({"regions":[]})") : j(R"({"markers":[]})"), "empty"}};
            c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<RulerList> {
                RulerList out;
                const auto s = app(ctx).view();
                for (const auto& e : model::Traits<Object>::map(s)) {
                    RulerEntry row;
                    row.timebase = e.value->timebase;
                    row.id = e.id.str();
                    row.name = e.value->name;
                    row.color = e.value->color;
                    if constexpr (Region) {
                        row.start = e.value->start;
                        row.end = e.value->end;
                        row.section = e.value->section;
                    } else {
                        row.pos = e.value->pos;
                    }
                    out.entries.push_back(std::move(row));
                }
                return out;
            };
            addChecked(registry, std::move(c));
        }
    };
    rulerCommands.template operator()<false>();
    rulerCommands.template operator()<true>();
    [&] {
        Command<TimeArgs, TimeResult> c;
        c.name = "time.convert";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Convert a seconds, beat, or bar position to all three time domains.";
        c.tags = {"time"};
        c.args = schema::object<TimeArgs>("time_convert_args")
                     .field("seconds", &TimeArgs::seconds, schema::number().min(0))
                     .field("beat", &TimeArgs::beat, schema::number().min(0))
                     .field("bar", &TimeArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .build();
        c.result = schema::object<TimeResult>("time_position")
                       .field("seconds", &TimeResult::seconds, schema::number())
                       .field("beat", &TimeResult::beat, schema::number())
                       .field("bar", &TimeResult::bar, schema::integer())
                       .field("beat_in_bar", &TimeResult::beatInBar, schema::integer())
                       .field("fraction", &TimeResult::fraction, schema::number())
                       .build();
        c.examples = {{"Bar two", j(R"({"bar":2})"),
                       j(R"({"seconds":2,"beat":4,"bar":2,"beat_in_bar":1,"fraction":0})"),
                       "empty"}};
        c.run = [](const TimeArgs& a, Context& ctx) -> Result<TimeResult> {
            if (a.seconds.has_value() + a.beat.has_value() + a.bar.has_value() != 1)
                return Error("time.input", "give exactly one of seconds, beat, or bar", "",
                             "remove the other time inputs");
            const auto s = app(ctx).view();
            const auto map = s.timeMap;
            const double beat = a.seconds ? map->beatAt(*a.seconds)
                                : a.bar   ? map->beatOfBar(static_cast<int>(*a.bar))
                                          : *a.beat;
            SUPPLE_TRY(model::checkedBeat(s, beat,
                                          a.seconds ? "/seconds"
                                          : a.bar   ? "/bar"
                                                    : "/beat"));
            const auto bb = map->barBeat(beat);
            return TimeResult{map->secondsAt(beat), beat, bb.bar, bb.beat, bb.fraction};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
