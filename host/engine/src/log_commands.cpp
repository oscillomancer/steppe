// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// log.list: the application log.
#include "extra_command_helpers.h"

namespace supple::engine {

void registerLogCommands(cmd::Registry& registry) {
    [&] {
        struct LogArgs {
            std::optional<std::int64_t> limit, since;
            bool queries = false;
            std::optional<std::string> origin;
        };
        Command<LogArgs, json::Value> c;
        c.name = "log.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Recent commands with origin, arguments and outcome (the command log).";
        c.description =
            "Newest last. Who did what: origin is gui, agent:<client>/<model>, mcp:<client>, "
            "lua:<script>, rpc. queries: include read-only commands (left out by default). since: "
            "only entries after this seq (poll with the last seq seen). origin: entries whose "
            "origin starts with this. The log keeps the last 2000 commands.";
        c.tags = {"history"};
        c.args = schema::object<LogArgs>("log_list_args")
                     .field("limit", &LogArgs::limit, schema::integer().range(1, 2000))
                     .field("since", &LogArgs::since, schema::integer().min(0))
                     .field("queries", &LogArgs::queries, schema::boolean())
                     .defaultValue(false)
                     .field("origin", &LogArgs::origin, schema::string())
                     .build();
        c.result = schema::any();
        c.examples = {{"The last commands", j(R"({"limit": 5})"),
                       j(R"({"entries":"<any>","last_seq":"<any>"})"), "song"}};
        c.run = [](const LogArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& engine = app(ctx);
            const cmd::Registry& commands = ctx.service<const cmd::Registry>();
            static constexpr const char* kTypes[] = {"ui",   "lua",      "mcp",  "rpc",
                                                     "test", "internal", "agent"};
            auto originOf = [](const cmd::Origin& o) {
                std::string s = kTypes[static_cast<int>(o.type)];
                if (!o.client.empty()) s += ":" + o.client;
                if (!o.agent.empty()) s += "/" + o.agent;
                return s;
            };
            std::vector<const cmd::LogRecord*> picked;
            const auto& records = engine.records();
            for (auto it = records.rbegin(); it != records.rend(); ++it) {
                if (a.since && it->seq <= static_cast<std::uint64_t>(*a.since)) break;
                if (!a.queries) {
                    const cmd::CommandInfo* info = commands.find(it->command, it->version);
                    if (info && info->kind == Kind::Query) continue;
                }
                const std::string origin = originOf(it->origin);
                if (a.origin && origin.rfind(*a.origin, 0) != 0) continue;
                picked.push_back(&*it);
                if (picked.size() >= static_cast<std::size_t>(a.limit.value_or(50))) break;
            }
            json::Value entries = json::Value::array();
            for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
                const cmd::LogRecord& r = **it;
                json::Value e = json::Value::object();
                e["seq"] = static_cast<std::int64_t>(r.seq);
                e["time_ms"] = static_cast<std::int64_t>(r.time_ms);
                e["origin"] = originOf(r.origin);
                e["command"] = r.command;
                e["args"] = r.args;
                e["ok"] = r.ok;
                if (!r.ok)
                    e["error"] = r.errorCode;
                else if (!r.resultDigest.empty())
                    e["result"] = r.resultDigest;
                e["duration_ms"] = std::round(static_cast<double>(r.duration_us) / 10.0) / 100.0;
                entries.push_back(std::move(e));
            }
            json::Value out = json::Value::object();
            out["entries"] = std::move(entries);
            out["last_seq"] = static_cast<std::int64_t>(records.empty() ? 0 : records.back().seq);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
