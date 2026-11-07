// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/base/closest.h>
#include <supple/cmd/bus.h>
#include <supple/product.h>
#include <algorithm>
#include <charconv>
#include <chrono>

namespace supple::cmd {
namespace {
std::string kindName(Kind kind) {
    switch (kind) {
        case Kind::Query:
            return "query";
        case Kind::Edit:
            return "edit";
        case Kind::Action:
            return "action";
    }
    return {};
}
std::string exposureName(Exposure exposure) {
    switch (exposure) {
        case Exposure::McpTool:
            return "mcp-tool";
        case Exposure::McpExec:
            return "mcp-exec";
        case Exposure::Internal:
            return "internal";
    }
    return {};
}
[[maybe_unused]] bool validName(std::string_view name) {
    if (name.empty()) return false;
    bool segmentStart = true;
    for (char ch : name) {
        if (ch == '.') {
            if (segmentStart) return false;
            segmentStart = true;
        } else if (segmentStart) {
            if (ch < 'a' || ch > 'z') return false;
            segmentStart = false;
        } else if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
            return false;
    }
    return !segmentStart;
}
json::Value exampleJson(const Example& example) {
    json::Value out = json::Value::object();
    out["title"] = example.title;
    out["args"] = example.args;
    out["result"] = example.result;
    out["fixture"] = example.fixture;
    return out;
}
json::Value commandJson(const CommandInfo& info) {
    json::Value out = json::Value::object();
    out["name"] = info.name;
    out["version"] = info.version;
    out["kind"] = kindName(info.kind);
    out["exposure"] = exposureName(info.exposure);
    out["summary"] = info.summary;
    out["description"] = info.description;
    out["tags"] = json::Value::array();
    for (const auto& tag : info.tags) out["tags"].push_back(tag);
    out["args"] = info.argsSchema;
    out["result"] = info.resultSchema;
    out["errors"] = json::Value::array();
    for (const auto& error : info.errors) {
        json::Value item = json::Value::object();
        item["code"] = error.code;
        item["when"] = error.when;
        item["hint"] = error.hint;
        out["errors"].push_back(std::move(item));
    }
    out["examples"] = json::Value::array();
    for (const auto& example : info.examples) out["examples"].push_back(exampleJson(example));
    return out;
}
json::Value objectSchema(std::initializer_list<std::pair<std::string, json::Value>> fields,
                         std::initializer_list<std::string> required = {}) {
    json::Value out = json::Value::object();
    out["type"] = "object";
    out["properties"] = json::Value::object();
    for (const auto& [name, schema] : fields) out["properties"][name] = schema;
    out["required"] = json::Value::array();
    for (const auto& name : required) out["required"].push_back(name);
    out["additionalProperties"] = false;
    return out;
}
json::Value typeSchema(std::string_view type) {
    json::Value out = json::Value::object();
    out["type"] = std::string(type);
    return out;
}
// Handler schemas stay strict; exported schemas also describe the diagnostics
// that dispatch attaches after a successful invocation/transaction.
json::Value warningSchema() {
    return typeSchema("object");  // the shared contract is documented once, outside tool schemas
}
json::Value warningArray() {
    auto out = typeSchema("array");
    out["items"] = warningSchema();
    return out;
}
json::Value replySchema(json::Value raw) {
    if (auto* alternatives = raw.find("oneOf"); alternatives && alternatives->isArray()) {
        for (auto& variant : raw["oneOf"].asArray()) variant = replySchema(std::move(variant));
        return raw;
    }
    if (auto* alternatives = raw.find("anyOf"); alternatives && alternatives->isArray()) {
        for (auto& variant : raw["anyOf"].asArray()) variant = replySchema(std::move(variant));
        return raw;
    }
    const auto* type = raw.find("type");
    if (!type || !type->isString()) return raw;  // any JSON, including replies with diagnostics
    if (type->asString() != "object") {
        auto wrapped =
            objectSchema({{"result", raw}, {"warnings", warningArray()}}, {"result", "warnings"});
        auto out = json::Value::object();
        out["oneOf"] = json::Value::array();
        out["oneOf"].push_back(std::move(raw));
        out["oneOf"].push_back(std::move(wrapped));
        return out;
    }
    if (!raw.find("properties")) raw["properties"] = json::Value::object();
    auto* existing = raw["properties"].find("warnings");
    auto combined = warningArray();
    if (existing) {
        const auto old = *existing;
        const auto* oldType = old.find("type");
        if (oldType && *oldType == "array") {
            combined = old;
            combined.asObject().erase("maxItems");
            combined.asObject().erase("uniqueItems");
            if (const auto* items = old.find("items")) {
                const auto* itemType = items->find("type");
                if (itemType && *itemType == "object") {
                    combined["items"] = warningSchema();
                } else {
                    combined["items"] = json::Value::object();
                    combined["items"]["anyOf"] = json::Value::array();
                    combined["items"]["anyOf"].push_back(*items);
                    combined["items"]["anyOf"].push_back(warningSchema());
                }
            }
        } else {
            // A command-owned scalar/object warning keeps its value. Only
            // warning-bearing replies turn it into the first array entry.
            combined["items"] = json::Value::object();
            combined["items"]["anyOf"] = json::Value::array();
            combined["items"]["anyOf"].push_back(old);
            combined["items"]["anyOf"].push_back(warningSchema());
            auto variants = json::Value::array();
            variants.push_back(old);
            variants.push_back(std::move(combined));
            combined = json::Value::object();
            combined["anyOf"] = std::move(variants);
        }
    }
    raw["properties"]["warnings"] = std::move(combined);
    return raw;
}
void appendWarnings(json::Value& result, const std::vector<Warning>& warnings) {
    if (warnings.empty()) return;
    if (!result.isObject()) {
        auto wrapped = json::Value::object();
        wrapped["result"] = std::move(result);
        result = std::move(wrapped);
    }
    if (!result.find("warnings")) result["warnings"] = json::Value::array();
    if (!result["warnings"].isArray()) {
        auto previous = std::move(result["warnings"]);
        result["warnings"] = json::Value::array();
        result["warnings"].push_back(std::move(previous));
    }
    const std::size_t commandWarnings = result["warnings"].size();
    for (const auto& warning : warnings) {
        auto& entries = result["warnings"].asArray();
        const bool alreadyReported = std::any_of(
            entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(commandWarnings),
            [&](const json::Value& entry) {
                const auto* code = entry.find("code");
                const auto* message = entry.find("message");
                const auto* path = entry.find("path");
                return code && message && path && *code == warning.code &&
                       *message == warning.message && *path == warning.path;
            });
        if (alreadyReported) continue;
        auto value = json::Value::object();
        value["code"] = warning.code;
        value["message"] = warning.message;
        value["path"] = warning.path;
        result["warnings"].push_back(std::move(value));
    }
}

std::string stringField(const json::Value& value, std::string_view key) {
    const auto* member = value.find(key);
    return member && member->isString() ? member->asString() : std::string{};
}
Error requiredField(std::string_view field) {
    return Error("schema.missing", "missing required field", json::pointerJoin("", field),
                 "provide \"" + std::string(field) + "\"");
}
Status validateFields(const json::Value& args, std::initializer_list<std::string_view> required,
                      std::initializer_list<std::string_view> allowed) {
    if (!args.isObject()) return Error("schema.type", "expected object", "", "use an object");
    std::vector<Error> errors;
    for (auto key : required) {
        const auto* value = args.find(key);
        if (!value)
            errors.push_back(requiredField(key));
        else if (!value->isString())
            errors.emplace_back("schema.type", "expected string", json::pointerJoin("", key),
                                "use a string");
    }
    for (const auto& member : args.asObject()) {
        bool known = false;
        for (auto key : allowed)
            if (member.key == key) known = true;
        if (!known)
            errors.emplace_back("schema.unknown_member", "unknown member",
                                json::pointerJoin("", member.key), "remove this member");
    }
    if (errors.empty()) return {};
    if (errors.size() == 1) return errors.front();
    Error error("schema.invalid", "invalid arguments");
    error.details = std::move(errors);
    return error;
}
Status validateBatch(const json::Value& args) {
    std::vector<Error> errors;
    auto closed = [&](const json::Value& object, const std::string& path,
                      std::initializer_list<std::string_view> allowed) {
        if (!object.isObject()) {
            errors.emplace_back("schema.type", "expected object", path, "use an object");
            return false;
        }
        for (const auto& member : object.asObject()) {
            if (std::find(allowed.begin(), allowed.end(), member.key) != allowed.end()) continue;
            const auto closest = closestMatch(member.key, {allowed.begin(), allowed.size()});
            errors.emplace_back(
                "schema.unknown_member", "unknown member", json::pointerJoin(path, member.key),
                closest.empty() ? "remove this member" : "did you mean \"" + closest + "\"?");
        }
        return true;
    };
    auto field = [&](const json::Value& object, const std::string& path, std::string_view key,
                     bool required, std::string_view type) {
        const auto* value = object.find(key);
        const auto memberPath = json::pointerJoin(path, key);
        if (!value) {
            if (required)
                errors.emplace_back("schema.missing", "missing required field", memberPath,
                                    "provide \"" + std::string(key) + "\"");
        } else if ((type == "string" && !value->isString()) ||
                   (type == "array" && !value->isArray()) ||
                   (type == "object" && !value->isObject())) {
            errors.emplace_back("schema.type", "expected " + std::string(type), memberPath,
                                "use " + std::string(type));
        }
    };
    if (closed(args, "", {"label", "steps"})) {
        field(args, "", "label", false, "string");
        field(args, "", "steps", true, "array");
        const auto* steps = args.find("steps");
        if (steps && steps->isArray()) {
            for (std::size_t i = 0; i < steps->size(); ++i) {
                const auto path = json::pointerJoin("/steps", std::to_string(i));
                const auto& step = steps->at(i);
                if (!closed(step, path, {"command", "args", "as"})) continue;
                field(step, path, "command", true, "string");
                field(step, path, "args", false, "object");
                field(step, path, "as", false, "string");
            }
        }
    }
    if (errors.empty()) return {};
    if (errors.size() == 1) return errors.front();
    Error error("schema.invalid", "invalid arguments");
    error.details = std::move(errors);
    return error;
}
std::string pointerPrefix(std::string_view prefix, std::string_view path) {
    // Only JSON pointers nest; a text position ("1:43", notation) stays as it is.
    if (!path.empty() && path.front() != '/') return std::string(path);
    return std::string(prefix) + std::string(path);
}
void prefixError(Error& error, std::string_view prefix) {
    error.path = pointerPrefix(prefix, error.path);
    for (auto& detail : error.details) prefixError(detail, prefix);
}
Result<std::pair<std::string, int>> parseCommand(std::string_view command) {
    const auto at = command.find('@');
    if (at == std::string_view::npos) return std::pair{std::string(command), 0};
    std::string name(command.substr(0, at));
    auto suffix = command.substr(at + 1);
    if (suffix.empty() || suffix.front() == '0')
        return Error("command.version", "invalid command version", "/command",
                     "use @ followed by a positive integer");
    int version = 0;
    for (char ch : suffix) {
        if (ch < '0' || ch > '9' || version > 1000000)
            return Error("command.version", "invalid command version", "/command",
                         "use @ followed by a positive integer");
        version = version * 10 + (ch - '0');
    }
    return std::pair{std::move(name), version};
}
std::string availableVersions(const Registry& registry, std::string_view name) {
    std::string result;
    for (auto* info : registry.all())
        if (info->name == name) {
            if (!result.empty()) result += ", ";
            result += std::to_string(info->version);
        }
    return result;
}
Result<json::Value> resolveReference(const json::Value& value,
                                     const std::map<std::string, json::Value>& saved,
                                     const std::string& path) {
    if (value.isString()) {
        auto text = value.asString();
        if (text.size() >= 4 && text.substr(0, 2) == "${" && text.back() == '}') {
            auto expr = text.substr(2, text.size() - 3);
            auto dot = expr.find('.');
            std::string name = expr.substr(0, dot);
            auto it = saved.find(name);
            const json::Value* current = it == saved.end() ? nullptr : &it->second;
            std::size_t pos = dot;
            while (current && pos != std::string::npos) {
                std::size_t next = expr.find('.', pos + 1);
                auto key = expr.substr(pos + 1, next == std::string::npos ? next : next - pos - 1);
                if (current->isObject())
                    current = current->find(key);
                else if (current->isArray()) {
                    if (key.empty() || !std::all_of(key.begin(), key.end(),
                                                    [](char ch) { return ch >= '0' && ch <= '9'; }))
                        current = nullptr;
                    else {
                        std::size_t index = 0;
                        auto [end, error] =
                            std::from_chars(key.data(), key.data() + key.size(), index);
                        current = error == std::errc{} && end == key.data() + key.size() &&
                                          index < current->size()
                                      ? &current->at(index)
                                      : nullptr;
                    }
                } else
                    current = nullptr;
                pos = next;
            }
            if (!current) {
                std::string names;
                for (const auto& [available, unused] : saved) {
                    (void)unused;
                    if (!names.empty()) names += ", ";
                    names += available;
                }
                return Error("batch.bad_reference", "unknown step reference \"" + expr + "\"", path,
                             "available names: " + (names.empty() ? "(none)" : names));
            }
            return *current;
        }
        return value;
    }
    if (value.isArray()) {
        json::Value out = json::Value::array();
        for (std::size_t i = 0; i < value.size(); ++i) {
            auto child =
                resolveReference(value.at(i), saved, json::pointerJoin(path, std::to_string(i)));
            if (!child) return child.error();
            out.push_back(std::move(*child));
        }
        return out;
    }
    if (value.isObject()) {
        json::Value out = json::Value::object();
        for (const auto& member : value.asObject()) {
            auto child = resolveReference(member.value, saved, json::pointerJoin(path, member.key));
            if (!child) return child.error();
            out[member.key] = std::move(*child);
        }
        return out;
    }
    return value;
}
bool matches(const json::Value& expected, const json::Value& actual) {
    if (expected.isString() && expected.asString() == "<any>") return true;
    if (expected.isArray() && actual.isArray() && expected.size() == actual.size()) {
        for (std::size_t i = 0; i < expected.size(); ++i)
            if (!matches(expected.at(i), actual.at(i))) return false;
        return true;
    }
    if (expected.isObject() && actual.isObject() && expected.size() == actual.size()) {
        for (const auto& member : expected.asObject()) {
            const auto* value = actual.find(member.key);
            if (!value || !matches(member.value, *value)) return false;
        }
        return true;
    }
    return expected == actual;
}
}  // namespace

Registry::Registry() {
    auto addMeta = [&](std::string name, Kind kind, Exposure exposure, std::string summary,
                       json::Value argsSchema, json::Value resultSchema,
                       std::function<Status(const json::Value&)> validate,
                       std::function<Result<json::Value>(const json::Value&, Context&)> invoke,
                       Example example) {
        CommandInfo info;
        info.name = std::move(name);
        info.kind = kind;
        info.exposure = exposure;
        info.summary = std::move(summary);
        info.description = info.summary;
        info.argsSchema = std::move(argsSchema);
        info.resultSchema = std::move(resultSchema);
        info.validate = std::move(validate);
        info.validateResult = [](const json::Value&) -> Status { return {}; };
        info.invoke = std::move(invoke);
        info.examples.push_back(std::move(example));
        addInfo(std::move(info));
    };
    auto string = typeSchema("string");
    auto anyObject = typeSchema("object");
    auto waitTimeout = typeSchema("number");
    waitTimeout["minimum"] = 0.0;
    waitTimeout["description"] =
        "Seconds to block (default 30). A larger value is clamped to 30 and reported as "
        "timeout_clamped_s.";
    addMeta(
        "registry.list", Kind::Query, Exposure::McpExec, "List available commands.",
        objectSchema({{"tag", string}, {"kind", string}, {"text", string}}), anyObject,
        [](const json::Value& args) -> Status {
            SUPPLE_TRY(validateFields(args, {}, {"tag", "kind", "text"}));
            for (auto key : {"tag", "kind", "text"})
                if (auto* value = args.find(key); value && !value->isString())
                    return Error("schema.type", "expected string", json::pointerJoin("", key));
            return {};
        },
        [](const json::Value& args, Context& ctx) -> Result<json::Value> {
            json::Value result = json::Value::object();
            result["commands"] = json::Value::array();
            auto tag = stringField(args, "tag"), kind = stringField(args, "kind");
            auto text = stringField(args, "text");
            for (auto* info : ctx.service<const Registry>().all()) {
                if (!tag.empty() &&
                    std::find(info->tags.begin(), info->tags.end(), tag) == info->tags.end())
                    continue;
                if (!kind.empty() && kindName(info->kind) != kind) continue;
                if (!text.empty() && info->name.find(text) == std::string::npos &&
                    info->summary.find(text) == std::string::npos)
                    continue;
                json::Value item = json::Value::object();
                item["name"] = info->name;
                item["version"] = info->version;
                item["kind"] = kindName(info->kind);
                item["summary"] = info->summary;
                result["commands"].push_back(std::move(item));
            }
            return result;
        },
        {"List commands", json::Value::object(), json::Value::object(), "empty"});
    addMeta(
        "registry.describe", Kind::Query, Exposure::McpExec, "Describe a command.",
        objectSchema({{"command", string}}, {"command"}), anyObject,
        [](const json::Value& args) { return validateFields(args, {"command"}, {"command"}); },
        [](const json::Value& args, Context& ctx) -> Result<json::Value> {
            auto parsed = parseCommand(stringField(args, "command"));
            if (!parsed) return parsed.error();
            auto* info = ctx.service<const Registry>().find(parsed->first, parsed->second);
            if (!info) {
                auto versions = availableVersions(ctx.service<const Registry>(), parsed->first);
                if (!versions.empty())
                    return Error("command.version", "unknown command version", "/command",
                                 "available versions: " + versions);
                return ctx.service<const Registry>().unknown(parsed->first);
            }
            return commandJson(*info);
        },
        {"Describe registry.list", json::Value::object(), json::Value::object(), "empty"});
    auto stepSchema =
        objectSchema({{"command", string}, {"args", anyObject}, {"as", string}}, {"command"});
    auto stepsSchema = typeSchema("array");
    stepsSchema["items"] = std::move(stepSchema);
    stepsSchema["maxItems"] = 256;
    addMeta("batch", Kind::Edit, Exposure::McpTool, "Run commands atomically.",
            objectSchema({{"label", string}, {"steps", stepsSchema}}, {"steps"}), anyObject,
            validateBatch,
            [](const json::Value&, Context&) -> Result<json::Value> {
                return Error("command.internal", "batch must run through Dispatcher");
            },
            {"Empty batch", json::Value::object(), json::Value::object(), "empty"});
    addMeta(
        "job.status", Kind::Query, Exposure::McpExec, "Get a job's status.",
        objectSchema({{"job", string}}, {"job"}), anyObject,
        [](const json::Value& args) { return validateFields(args, {"job"}, {"job"}); },
        [](const json::Value& args, Context& ctx) {
            return ctx.service<JobManager>().status(stringField(args, "job"));
        },
        {"Get job", json::Value::object(), json::Value::object(), "job_running"});
    addMeta(
        "job.get", Kind::Query, Exposure::McpExec, "Get a job's status and progress.",
        objectSchema({{"job", string}}, {"job"}), anyObject,
        [](const json::Value& args) { return validateFields(args, {"job"}, {"job"}); },
        [](const json::Value& args, Context& ctx) {
            return ctx.service<JobManager>().status(stringField(args, "job"));
        },
        {"Get render job", json::Value::object(), json::Value::object(), "job_running"});
    addMeta(
        "job.wait", Kind::Query, Exposure::McpExec, "Wait for a job.",
        objectSchema({{"job", string}, {"timeout_s", waitTimeout}}, {"job"}), anyObject,
        [](const json::Value& args) -> Status {
            SUPPLE_TRY(validateFields(args, {"job"}, {"job", "timeout_s"}));
            if (auto* timeout = args.find("timeout_s");
                timeout && (!timeout->isNumber() || timeout->asDouble() < 0))
                return Error("schema.out_of_range",
                             "timeout_s must be a nonnegative number of seconds", "/timeout_s",
                             "a value above 30 is clamped to 30");
            return {};
        },
        [](const json::Value& args, Context& ctx) -> Result<json::Value> {
            const auto* timeout = args.find("timeout_s");
            const double requested = timeout ? timeout->asDouble() : kMaxWaitSeconds;
            const double seconds = std::min(requested, kMaxWaitSeconds);
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(seconds));
            auto& jobs = ctx.service<JobManager>();
            for (;;) {
                // Worker completions can need model-thread publication/import.
                // Service posted work during the wait so it can finish the job.
                ctx.service<Dispatcher>().flushJobEvents();
                auto status = jobs.wait(stringField(args, "job"), 0);
                if (!status) return status.error();
                const auto remaining =
                    std::chrono::duration<double>(until - std::chrono::steady_clock::now()).count();
                if (*status->find("status") != "running" || remaining <= 0) {
                    if (requested > kMaxWaitSeconds)
                        (*status)["timeout_clamped_s"] = kMaxWaitSeconds;
                    return status;
                }
                auto waited = jobs.wait(stringField(args, "job"), std::min(0.01, remaining));
                if (!waited) return waited.error();
            }
        },
        {"Wait for job", json::Value::object(), json::Value::object(), "job_running"});
    addMeta(
        "job.cancel", Kind::Action, Exposure::McpExec, "Cancel a job.",
        objectSchema({{"job", string}}, {"job"}), anyObject,
        [](const json::Value& args) { return validateFields(args, {"job"}, {"job"}); },
        [](const json::Value& args, Context& ctx) -> Result<json::Value> {
            auto& jobs = ctx.service<JobManager>();
            SUPPLE_TRY(jobs.cancel(stringField(args, "job")));
            return jobs.status(stringField(args, "job"));
        },
        {"Cancel job", json::Value::object(), json::Value::object(), "job_running"});
    addMeta(
        "job.list", Kind::Query, Exposure::McpExec, "List session jobs.", objectSchema({}),
        anyObject, [](const json::Value& args) { return validateFields(args, {}, {}); },
        [](const json::Value&, Context& ctx) -> Result<json::Value> {
            return ctx.service<JobManager>().list();
        },
        {"List jobs", json::Value::object(), json::Value::object(), "empty"});
    auto& listExample = commands_.at({"registry.list", 1}).examples.front();
    listExample.args["text"] = "registry.list";
    listExample.result["commands"] = json::Value::array();
    json::Value listed = json::Value::object();
    const auto& self = commands_.at({"registry.list", 1});
    listed["name"] = self.name;
    listed["version"] = self.version;
    listed["kind"] = kindName(self.kind);
    listed["summary"] = self.summary;
    listExample.result["commands"].push_back(std::move(listed));
    auto& describe = commands_.at({"registry.describe", 1}).examples.front();
    describe.args["command"] = "registry.list";
    describe.result = commandJson(commands_.at({"registry.list", 1}));
    auto& batchExample = commands_.at({"batch", 1}).examples.front();
    batchExample.args["steps"] = json::Value::array();
    batchExample.result["label"] = "";
    batchExample.result["results"] = json::Value::array();
    for (auto name : {"job.status", "job.get", "job.wait", "job.cancel"}) {
        auto& ex = commands_.at({name, 1}).examples.front();
        ex.args["job"] = "jb_1";
        ex.result["job"] = "jb_1";
        ex.result["status"] = name == std::string_view("job.cancel") ? "cancelled" : "running";
        ex.result["progress"] = json::Value::object();
        ex.result["progress"]["fraction"] = 0.0;
        ex.result["progress"]["message"] = "";
        if (name == std::string_view("job.wait")) ex.args["timeout_s"] = 0;
    }
    // job.wait says what a large timeout_s does (the arg is clamped, not rejected).
    commands_.at({"job.wait", 1}).description =
        "Wait for a job. Blocks for up to timeout_s seconds (default 30) until the job finishes; "
        "a larger timeout_s is accepted and clamped to 30, and the result then carries "
        "\"timeout_clamped_s\": 30. Otherwise the result is what job.status returns.";
    commands_.at({"job.list", 1}).examples.front().result["jobs"] = json::Value::array();
}
void Registry::addInfo(CommandInfo info) {
    info.resultSchema = replySchema(std::move(info.resultSchema));
    assert(validName(info.name) && info.version > 0 && info.summary.size() <= 80);
    assert(info.exposure == Exposure::Internal || !info.examples.empty());
    auto key = std::pair{info.name, info.version};
    assert(!commands_.contains(key));
    commands_.emplace(std::move(key), std::move(info));
}
const CommandInfo* Registry::find(std::string_view name, int version) const {
    if (version) {
        auto it = commands_.find({std::string(name), version});
        return it == commands_.end() ? nullptr : &it->second;
    }
    const CommandInfo* found = nullptr;
    auto it = commands_.lower_bound({std::string(name), 0});
    while (it != commands_.end() && it->first.first == name) {
        found = &it->second;
        ++it;
    }
    return found;
}
std::vector<const CommandInfo*> Registry::all() const {
    std::vector<const CommandInfo*> out;
    for (const auto& [key, value] : commands_) {
        (void)key;
        out.push_back(&value);
    }
    return out;
}
Error Registry::unknown(std::string_view name) const {
    std::vector<std::string_view> names;
    std::string last;
    for (auto* info : all())
        if (info->name != last) {
            names.push_back(info->name);
            last = info->name;
        }
    std::string hint;
    for (int i = 0; i < 3 && !names.empty(); ++i) {
        auto choice = closestMatch(name, names);
        if (choice.empty()) break;
        if (!hint.empty()) hint += ", ";
        hint += choice;
        names.erase(std::remove(names.begin(), names.end(), choice), names.end());
    }
    return Error("command.unknown", "unknown command \"" + std::string(name) + "\"", "/command",
                 hint.empty() ? "use registry.list" : "did you mean " + hint + "?");
}
json::Value Registry::exportJson() const {
    json::Value out = json::Value::object();
    out["schema"] = "supple.command-registry/1.0";
    out["product"] = std::string(product::kName);
    out["version"] = std::string(product::kVersion);
    out["commands"] = json::Value::object();
    for (auto* info : all())
        out["commands"][info->name + "@" + std::to_string(info->version)] = commandJson(*info);
    return out;
}
std::uint64_t Host::clockMs() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

Dispatcher::~Dispatcher() {
    host_.stopJobs();
    host_.dispatcherAttached(nullptr);
}

Result<json::Value> Dispatcher::guarded(Admission admission,
                                        const std::function<Result<json::Value>()>& body) {
    admissions_.push_back(std::move(admission));
    struct Pop {
        std::vector<Admission>& entries;
        ~Pop() { entries.pop_back(); }
    } pop{admissions_};
    return body();
}

Result<json::Value> Dispatcher::execute(const Invocation& invocation) {
    auto result = dispatch(invocation, false);
    flushJobEvents();
    host_.commandFinished();
    return result;
}
void Dispatcher::post(std::string label, std::function<Status()> work, PostKind kind) {
    {
        std::lock_guard lock(postedMutex_);
        posted_.push_back({std::move(label), std::move(work), kind});
    }
    host_.workQueued();
}

void Dispatcher::flushJobEvents() {
    // Posted work first (it may execute commands, which flush again: the queue
    // is taken before running so nothing runs twice).
    for (;;) {
        std::vector<Posted> work;
        {
            std::lock_guard lock(postedMutex_);
            work.swap(posted_);
            if (transactionDepth_ > 0) {
                // Edits wait for the open transaction to close; they keep
                // their place ahead of anything posted later.
                auto edits = std::stable_partition(work.begin(), work.end(), [](const Posted& p) {
                    return p.kind != PostKind::Edit;
                });
                posted_.insert(posted_.begin(), std::make_move_iterator(edits),
                               std::make_move_iterator(work.end()));
                work.erase(edits, work.end());
            }
        }
        if (work.empty()) break;
        for (auto& [label, fn, kind] : work)
            if (auto done = fn(); !done) {
                json::Value payload = json::Value::object();
                payload["label"] = label;
                payload["error"] = done.error().toString();
                host_.emit("bus.post_failed", std::move(payload));
            }
    }
    for (auto& [topic, payload] : jobs_.drainEvents()) host_.emit(topic, std::move(payload));
}
Result<json::Value> Dispatcher::transact(std::string_view label, const Origin& origin,
                                         const std::function<Result<json::Value>()>& body) {
    if (transactionDepth_ > 0) return body();  // join the transaction already open
    auto transaction = host_.beginTransaction(origin, label);
    if (!transaction) return transaction.error();
    transactionFailure_.reset();
    ++transactionDepth_;
    auto result = body();
    --transactionDepth_;
    if (auto failure = std::exchange(transactionFailure_, {})) {
        Error error("transaction.restore_failed", "an invocation could not be restored", {},
                    "repair the reported host state before retrying the transaction");
        error.details.push_back(std::move(*failure));
        if (!result) error.details.push_back(result.error());
        result = std::move(error);
    }
    if (!result) {
        auto rollback = host_.rollbackTransaction(*transaction);
        if (!rollback) {
            auto error = rollback.error();
            error.details.push_back(result.error());
            return error;
        }
        return result;
    }
    auto committed = host_.commitTransaction(*transaction);
    if (!committed) {
        auto rollback = host_.rollbackTransaction(*transaction);
        if (!rollback) {
            auto error = rollback.error();
            error.details.push_back(committed.error());
            return error;
        }
        return committed.error();
    }
    return result;
}
Result<json::Value> Dispatcher::dispatch(const Invocation& invocation, bool inBatch, bool) {
    for (const auto& admit : admissions_)
        if (auto allowed = admit(invocation); !allowed) return allowed.error();
    inBatch = inBatch || transactionDepth_ > 0;
    if (inBatch && transactionFailure_) return *transactionFailure_;
    const auto start = std::chrono::steady_clock::now();
    auto parsed = parseCommand(invocation.command);
    auto name = parsed ? parsed->first : invocation.command;
    auto* info = parsed ? registry_.find(name, parsed->second) : nullptr;
    std::optional<json::Value> resolvedArgs;
    auto record = [&](const Result<json::Value>& result) {
        LogRecord log;
        log.seq = ++sequence_;
        log.time_ms = host_.clockMs();
        log.origin = invocation.origin;
        log.command = name;
        log.version = info ? info->version : parsed ? parsed->second : 0;
        log.args = resolvedArgs.value_or(invocation.args);
        log.ok = result.ok();
        if (result) {
            log.resultDigest = json::writeCompact(*result).substr(0, 200);
        } else {
            log.errorCode = result.error().code;
        }
        log.duration_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
        host_.log(log);
    };
    auto finish = [&](Result<json::Value> result) {
        record(result);
        return result;
    };
    if (!parsed) return finish(parsed.error());
    if (!info) {
        auto versions = availableVersions(registry_, name);
        if (!versions.empty())
            return finish(Error("command.version", "unknown version of \"" + name + "\"",
                                "/command", "available versions: " + versions));
        return finish(registry_.unknown(name));
    }
    auto validation = info->validate(invocation.args);
    if (!validation) {
        Error error("command.invalid_args", "invalid arguments for " + name, "/args",
                    "fix the listed argument errors");
        auto inner = validation.error();
        if (inner.details.empty()) {
            prefixError(inner, "/args");
            error.details.push_back(std::move(inner));
        } else {
            for (auto& detail : inner.details) {
                prefixError(detail, "/args");
                error.details.push_back(std::move(detail));
            }
        }
        return finish(std::move(error));
    }
    if (inBatch && info->kind == Kind::Action && !host_.actionTransactional(name))
        return finish(Error("batch.not_transactional", "action cannot run inside a transaction",
                            "/command", "run it outside batch or make the host transaction-safe"));
    if (name == "batch") {
        if (inBatch)
            return finish(
                Error("batch.not_transactional", "nested batch is unsupported", "/command"));
        return finish(batch(invocation.args, invocation.origin));
    }
    Context context = host_.makeContext(invocation.origin);
    context.provide<const Registry>(registry_);
    context.provide<JobManager>(jobs_);
    context.provide<Dispatcher>(*this);
    auto invoke = [&]() -> Result<json::Value> {
        auto saved = inBatch ? host_.invocationSavepoint()
                             : Result<std::function<Status()>>(std::function<Status()>{});
        if (!saved) return saved.error();
        auto fail = [&](Error error) -> Result<json::Value> {
            if (*saved) {
                auto restored = (*saved)();
                if (!restored) {
                    if (!transactionFailure_) transactionFailure_ = restored.error();
                    error.details.push_back(restored.error());
                }
            }
            return error;
        };
        auto result = info->invoke(invocation.args, context);
        resolvedArgs = context.resolvedArgs();
        if (!result) {
            auto error = result.error();
            if (!error.path.empty() && error.path.front() == '/' &&
                error.path.rfind("/args", 0) != 0)
                prefixError(error, "/args");
            return fail(std::move(error));
        }
        auto checked = info->validateResult(*result);
        if (!checked) {
            Error error("command.bad_result", "handler returned a value outside its result schema",
                        "/result", "fix the command handler or result schema");
            error.details.push_back(checked.error());
            return fail(std::move(error));
        }
        if (info->kind != Kind::Query)
            host_.invoked(name, info->version, resolvedArgs.value_or(invocation.args), info->kind);
        return result;
    };
    Result<json::Value> result = Error("command.internal", "unreachable");
    if (info->kind == Kind::Edit && !inBatch) {
        result = transact(name, invocation.origin, invoke);
    } else
        result = invoke();
    if (result) appendWarnings(*result, context.warnings());
    return finish(std::move(result));
}
Result<json::Value> Dispatcher::batch(const json::Value& args, const Origin& origin) {
    const auto& steps = *args.find("steps");
    if (steps.size() > 256)
        return Error("batch.too_many_steps", "batch has more than 256 steps", "/steps",
                     "split the batch into at most 256 steps");
    // Resolve all command names and transaction constraints before any handler runs.
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.at(i);
        auto path = json::pointerJoin("/steps", std::to_string(i));
        auto parsed = parseCommand(step.find("command")->asString());
        if (!parsed) {
            auto error = parsed.error();
            prefixError(error, path);
            return error;
        }
        auto* info = registry_.find(parsed->first, parsed->second);
        if (!info) {
            auto versions = availableVersions(registry_, parsed->first);
            auto error = versions.empty() ? registry_.unknown(parsed->first)
                                          : Error("command.version", "unknown command version",
                                                  "/command", "available versions: " + versions);
            prefixError(error, path);
            return error;
        }
        if (info->name == "batch" ||
            (info->kind == Kind::Action && !host_.actionTransactional(info->name)))
            return Error("batch.not_transactional", "step cannot run inside a transaction", path,
                         "remove this action or make it transaction-safe in the host");
    }
    auto label = stringField(args, "label");
    return transact(label, origin, [&]() -> Result<json::Value> {
        json::Value result = json::Value::object();
        result["label"] = label;
        result["results"] = json::Value::array();
        std::map<std::string, json::Value> saved;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const auto& step = steps.at(i);
            auto path = json::pointerJoin("/steps", std::to_string(i));
            const auto* rawArgs = step.find("args");
            auto resolved =
                resolveReference(rawArgs ? *rawArgs : json::Value::object(), saved, path + "/args");
            if (!resolved) return resolved.error();
            Invocation invocation{step.find("command")->asString(), std::move(*resolved), origin};
            auto output = dispatch(invocation, true, true);
            if (!output) {
                auto detail = output.error();
                prefixError(detail, path);
                Error error("batch.step_failed", "batch step " + std::to_string(i) + " failed",
                            path, "fix this step and retry the batch");
                error.details.push_back(std::move(detail));
                return error;
            }
            if (auto* alias = step.find("as"); alias) saved[alias->asString()] = *output;
            result["results"].push_back(std::move(*output));
        }
        return result;
    });
}

Status runExamples(const Registry& registry, const FixtureFactory& factory,
                   const FixtureAvailable& available) {
    for (auto* info : registry.all()) {
        if (info->exposure == Exposure::Internal) continue;
        for (const auto& example : info->examples) {
            if (available && !available(example.fixture)) continue;
            auto host = factory(example.fixture);
            if (!host)
                return Error("command.example", "unknown example fixture", "",
                             "provide fixture \"" + example.fixture + "\"");
            Dispatcher dispatcher(registry, *host);
            host->setupFixture(dispatcher, example.fixture);
            auto output = dispatcher.execute({info->name + "@" + std::to_string(info->version),
                                              example.args,
                                              {Origin::Type::Test, "examples", {}}});
            if (!output)
                return Error("command.example",
                             "example failed: " + info->name + ": " + output.error().toString(), "",
                             "update the example or handler");
            if (!matches(example.result, *output))
                return Error("command.example", "example result differs: " + info->name, "",
                             "update the expected result or handler");
        }
    }
    return {};
}
json::Value mcpToolList(const Registry& registry) {
    json::Value tools = json::Value::array();
    for (auto* info : registry.all()) {
        if (info->exposure != Exposure::McpTool || registry.find(info->name) != info) continue;
        json::Value tool = json::Value::object();
        std::string name = info->name;
        std::replace(name.begin(), name.end(), '.', '_');
        tool["name"] = name;
        tool["title"] = info->summary;
        tool["description"] =
            info->summary + " " + info->description + " See describe for errors and examples.";
        auto input = info->argsSchema;
        if (auto* properties = input.find("properties"); properties && properties->isObject())
            for (auto& field : input["properties"].asObject()) {
                auto* unit = field.value.find("x-unit");
                if (!unit || !unit->isString()) continue;
                auto* description = field.value.find("description");
                std::string updated = description && description->isString()
                                          ? description->asString() + " "
                                          : std::string{};
                updated += "(" + unit->asString() + "; accepts quantity strings)";
                field.value["description"] = std::move(updated);
            }
        tool["inputSchema"] = std::move(input);
        // MCP output schemas must describe an object; results declared as any
        // JSON value (an empty schema) advertise none.
        if (const auto* type = info->resultSchema.find("type");
            type && type->isString() && type->asString() == "object")
            tool["outputSchema"] = info->resultSchema;
        tool["annotations"] = json::Value::object();
        tool["annotations"]["readOnlyHint"] = info->kind == Kind::Query;
        tool["annotations"]["destructiveHint"] = info->kind == Kind::Action;
        tool["annotations"]["idempotentHint"] =
            info->name.ends_with(".set") || info->name.ends_with(".merge");
        tools.push_back(std::move(tool));
    }
    return tools;
}
std::string markdownReference(const Registry& registry) {
    std::string text =
        "# Command reference\n\nGenerated from the command registry.\n"
        "\nSuccessful replies may include `warnings`: command warnings followed by bus "
        "diagnostics `{code: string, message: string, path: string}`. Paths are retained as "
        "supplied by the decoder or "
        "handler. "
        "Batch steps carry their own warnings in `results[i].warnings`. A non-object "
        "result with bus warnings is `{result: <value>, warnings: [...]}`. "
        "Replies without bus warnings keep their original shape.\n";
    for (auto* info : registry.all()) {
        text += "\n## `" + info->name + "@" + std::to_string(info->version) + "`\n\n";
        text += info->summary + "\n\n" + info->description + "\n\n";
        text += "Kind: `" + kindName(info->kind) + "`. Exposure: `" + exposureName(info->exposure) +
                "`.\n\n";
        text += "### Arguments\n\n| Field | Type | Unit | Range | Default | Description |\n";
        text += "| --- | --- | --- | --- | --- | --- |\n";
        if (auto* properties = info->argsSchema.find("properties");
            properties && properties->isObject())
            for (const auto& field : properties->asObject()) {
                const auto& schema = field.value;
                auto* type = schema.find("type");
                auto* unit = schema.find("x-unit");
                auto* minimum = schema.find("minimum");
                auto* maximum = schema.find("maximum");
                if (auto* variants = schema.find("anyOf");
                    variants && variants->isArray() && variants->size()) {
                    minimum = variants->at(0).find("minimum");
                    maximum = variants->at(0).find("maximum");
                }
                auto* fallback = schema.find("default");
                auto* description = schema.find("description");
                std::string range;
                if (minimum) range += json::writeCompact(*minimum);
                if (minimum || maximum) range += "..";
                if (maximum) range += json::writeCompact(*maximum);
                text += "| `" + field.key + "` | " +
                        (type && type->isString() ? type->asString() : "value") + " | " +
                        (unit && unit->isString() ? unit->asString() : "") + " | " + range + " | " +
                        (fallback ? json::writeCompact(*fallback) : "") + " | " +
                        (description && description->isString() ? description->asString() : "") +
                        " |\n";
            }
        text += "\n### Result\n\n```json\n" + json::writeCanonical(info->resultSchema) + "\n```\n";
        if (!info->errors.empty()) {
            text += "\n### Errors\n\n";
            for (const auto& error : info->errors)
                text += "- `" + error.code + "`: " + error.when + " Hint: " + error.hint + "\n";
        }
        text += "\n### Examples\n";
        for (const auto& example : info->examples)
            text += "\n**" + example.title + "** (`" + example.fixture + "`)\n\n```json\n" +
                    json::writeCanonical(example.args) + "\n```\n\nResult:\n\n```json\n" +
                    json::writeCanonical(example.result) + "\n```\n";
    }
    return text;
}
schema::Schema<Ref> refSchema() {
    json::Value exported = json::Value::object();
    exported["oneOf"] = json::Value::array();
    exported["oneOf"].push_back(typeSchema("string"));
    exported["oneOf"].push_back(typeSchema("object"));
    return schema::Schema<Ref>(
        "ref",
        [](const json::Value& value, Ref& out, schema::DecodeCtx& ctx, const std::string& path) {
            if (!value.isString() && !value.isObject()) {
                ctx.error("schema.type", "reference must be an ID or selector", path,
                          "use an ID string or selector object");
                return false;
            }
            out.value = value;
            return true;
        },
        [](const Ref& ref) { return ref.value; }, exported);
}
Result<std::string> resolveRef(Context& context, std::string_view kind, const Ref& ref) {
    if (ref.value.isString()) return ref.value.asString();
    return context.service<RefResolver>().resolve(kind, ref);
}

namespace {
// ---------------------------------------------------------------- LuaLS stubs
// The annotations the script bindings (libs/lua-bus) have: `q` and its
// namespaces, one class per command's arguments and result. Everything here is
// generated from the same JSON Schema an agent sees through `describe`, so it
// cannot drift from the bus.
std::string luaIdentifier(std::string_view name) {
    std::string out;
    for (char ch : name) out += (ch == '.' || ch == '-') ? '_' : ch;
    return out;
}
// A dotted table key becomes ["quoted"] when it is not a Lua identifier.
std::string luaFieldName(std::string_view name) {
    bool plain = !name.empty() && !(name.front() >= '0' && name.front() <= '9');
    for (char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_'))
            plain = false;
    return plain ? std::string(name) : "[\"" + std::string(name) + "\"]";
}
std::string oneLine(std::string_view text) {
    std::string out;
    bool space = false;
    for (char ch : text) {
        const bool blank = ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ';
        if (blank) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += ch;
    }
    return out;
}
std::string quoteLua(std::string_view text) {
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    out += '"';
    return out;
}
const std::string kLuaNamespace = luaIdentifier(product::kName);

// JSON Schema → a LuaLS type expression. Nested objects collapse to `table`:
// naming every anonymous sub-object is not worth the noise.
std::string luaType(const json::Value& schema, const json::Value& root,
                    const json::Value* scope = nullptr, int depth = 0) {
    if (depth >= 32) return "any";
    if (schema.find("$defs")) scope = &schema;
    if (!schema.isObject()) return "any";
    if (const auto* ref = schema.find("$ref"); ref && ref->isString()) {
        auto target = json::resolveFragment(root, ref->asString());
        if (!target && scope) target = json::resolveFragment(*scope, ref->asString());
        return target ? luaType(*target, root, scope, depth + 1) : "any";
    }
    if (const auto* choices = schema.find("enum");
        choices && choices->isArray() && choices->size()) {
        std::string out;
        for (const auto& choice : choices->asArray()) {
            if (!out.empty()) out += "|";
            out += choice.isString() ? quoteLua(choice.asString()) : "string";
        }
        return out;
    }
    if (const auto* variants = schema.find("oneOf"); variants && variants->isArray()) {
        bool allObjects = true;
        for (const auto& alternative : variants->asArray()) {
            const auto* type = alternative.find("type");
            if (!type || !type->isString() || type->asString() != "object") allObjects = false;
        }
        return allObjects ? "table" : "string|table";
    }
    if (const auto* variants = schema.find("anyOf"); variants && variants->isArray()) {
        std::vector<std::string> types;
        for (const auto& alternative : variants->asArray()) {
            auto type = luaType(alternative, root, scope, depth + 1);
            if (type == "any") return type;
            if (std::find(types.begin(), types.end(), type) == types.end())
                types.push_back(std::move(type));
        }
        std::string out;
        for (const auto& type : types) {
            if (!out.empty()) out += "|";
            out += type;
        }
        return out.empty() ? "any" : out;
    }
    const auto* type = schema.find("type");
    if (!type) return "any";
    if (type->isArray()) {
        std::string out;
        for (const auto& alternative : type->asArray()) {
            if (!alternative.isString()) continue;
            if (!out.empty()) out += "|";
            out += alternative.asString();
        }
        return out.empty() ? "any" : out;
    }
    if (!type->isString()) return "any";
    const std::string& name = type->asString();
    if (name == "array") {
        const auto* items = schema.find("items");
        auto itemType = items ? luaType(*items, root, scope, depth + 1) : std::string("any");
        if (itemType.find('|') != std::string::npos) itemType = "(" + itemType + ")";
        return itemType + "[]";
    }
    if (name == "object") {
        if (const auto* extra = schema.find("additionalProperties"); extra && extra->isObject())
            return "table<string, " + luaType(*extra, root, scope, depth + 1) + ">";
        return "table";
    }
    if (name == "integer" || name == "number" || name == "string" || name == "boolean") return name;
    return "any";
}
bool hasProperties(const json::Value& schema) {
    const auto* properties = schema.find("properties");
    if (!properties || !properties->isObject() || properties->size() == 0) return false;
    // Common metadata alone must not narrow an otherwise open result table
    // (registry.list, for example, still has arbitrary business fields).
    const auto* additional = schema.find("additionalProperties");
    if (properties->size() == 1 && properties->find("warnings") &&
        (!additional || !additional->isBool() || additional->asBool()))
        return false;
    return true;
}
std::string fieldDoc(const json::Value& schema) {
    std::string doc;
    if (const auto* description = schema.find("description");
        description && description->isString())
        doc = oneLine(description->asString());
    if (const auto* fallback = schema.find("default"))
        doc += (doc.empty() ? "" : " ") + std::string("(default: ") +
               json::writeCompact(*fallback) + ")";
    return doc;
}
// One class per object schema: the arguments or the result of a command.
std::string luaClass(const std::string& name, const json::Value& schema) {
    std::string out = "---@class " + name + "\n";
    const auto* required = schema.find("required");
    auto isRequired = [&](const std::string& field) {
        if (!required || !required->isArray()) return false;
        for (const auto& item : required->asArray())
            if (item.isString() && item.asString() == field) return true;
        return false;
    };
    if (const auto* properties = schema.find("properties"); properties && properties->isObject())
        for (const auto& field : properties->asObject()) {
            out += "---@field " + luaFieldName(field.key) + (isRequired(field.key) ? " " : "? ") +
                   luaType(field.value, schema);
            const std::string doc = fieldDoc(field.value);
            if (!doc.empty()) out += " " + doc;
            out += "\n";
        }
    return out;
}
// The `fun(...): R` signature of one command.
std::string luaSignature(const CommandInfo& info) {
    std::string args;
    if (hasProperties(info.argsSchema)) {
        bool anyRequired = false;
        if (const auto* required = info.argsSchema.find("required");
            required && required->isArray())
            anyRequired = required->size() > 0;
        args = "args" + std::string(anyRequired ? ": " : "?: ") + kLuaNamespace + "." +
               luaIdentifier(info.name) + "_args";
    } else if (const auto* properties = info.argsSchema.find("properties");
               properties && properties->isObject() && properties->size() > 0) {
        args = "args?: table";
    }
    const std::string result = hasProperties(info.resultSchema)
                                   ? kLuaNamespace + "." + luaIdentifier(info.name) + "_result"
                                   : "table";
    return "fun(" + args + "): " + result;
}
}  // namespace

std::string luaStubs(const Registry& registry) {
    std::string text = "---@meta\n";
    text += "-- " + std::string(product::kName) + " " + std::string(product::kVersion) +
            " scripting reference, generated from the command registry.\n";
    text +=
        "-- Do not edit: `<slug>-engine registry --lua` writes it, "
        "tools/gen/update-generated.sh regenerates it.\n";
    text +=
        "-- Successful replies can include .warnings: command warnings then {code,message,path}\n";
    text +=
        "-- bus diagnostics. Non-object results with bus warnings become "
        "{result=...,warnings=...}.\n";
    text += "-- The `q` table of a script (libs/lua-bus, ADR-0006): every command is a\n";
    text +=
        "-- function, `q.run(name, args)` reaches any of them by name, "
        "`q.batch(label, body)`\n";
    text += "-- is one transaction, and `q.on(topic, handler)` subscribes to bus events.\n\n";
    text += "---@type " + kLuaNamespace + "\nq = nil\n\n";

    // Arguments and results first, in registry order (name, then version).
    for (auto* info : registry.all()) {
        if (info->name == "batch") continue;              // q.batch is the transaction
        if (registry.find(info->name) != info) continue;  // only the newest version
        if (hasProperties(info->argsSchema))
            text += luaClass(kLuaNamespace + "." + luaIdentifier(info->name) + "_args",
                             info->argsSchema) +
                    "\n";
        if (hasProperties(info->resultSchema))
            text += luaClass(kLuaNamespace + "." + luaIdentifier(info->name) + "_result",
                             info->resultSchema) +
                    "\n";
    }

    // One class per namespace of the bindings: q.fx.param.set is a field of
    // product.fx.param. The names come from the command names, so a namespace
    // and a command of the same name would collide (none does today).
    std::map<std::string, std::vector<const CommandInfo*>> namespaces;
    std::vector<const CommandInfo*> roots;
    for (auto* info : registry.all()) {
        if (info->name == "batch") continue;
        if (registry.find(info->name) != info) continue;
        const auto dot = info->name.rfind('.');
        if (dot == std::string::npos)
            roots.push_back(info);
        else
            namespaces[info->name.substr(0, dot)].push_back(info);
    }
    for (const auto& [path, commands] : namespaces) {
        text += "---@class " + kLuaNamespace + "." + path + "\n";
        for (const auto* info : commands)
            text += "---@field " + luaFieldName(info->name.substr(info->name.rfind('.') + 1)) +
                    " " + luaSignature(*info) + " " + oneLine(info->summary) + "\n";
        text += "\n";
    }

    text += "---@class " + kLuaNamespace + "\n";
    for (const auto& [path, commands] : namespaces) {
        (void)commands;
        const auto dot = path.find('.');
        if (dot != std::string::npos) continue;  // only the top level is a field of q
        text += "---@field " + luaFieldName(path) + " " + kLuaNamespace + "." + path +
                " Commands named " + path + ".*\n";
    }
    for (const auto* info : roots)
        text += "---@field " + luaFieldName(info->name) + " " + luaSignature(*info) + " " +
                oneLine(info->summary) + "\n";
    text +=
        "---@field run fun(name: string, args?: table): any Run any registered command by "
        "name, e.g. the ones whose names are not Lua identifiers.\n";
    text +=
        "---@field batch fun(label: string, body: fun()) Run the calls inside body as one "
        "transaction: one undo step, rolled back when one of them fails.\n";
    text +=
        "---@field on fun(topic: string, handler: fun(payload: table, topic: string), "
        "filter?: table): integer Subscribe to bus events; returns a handle for q.off.\n";
    text += "---@field off fun(handle: integer) Drop a subscription.\n";
    text += "---@field log fun(...) Print a line to the script's log (the same as print).\n";
    text += "---@field null any The JSON null: Lua tables cannot hold nil.\n";
    text += "---@field name string The name this script runs under (its origin is `lua:<name>`).\n";
    return text;
}
}  // namespace supple::cmd
