// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <supple/base/result.h>
#include <supple/json/json.h>
#include <supple/schema/schema.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace supple::cmd {

enum class Kind : std::uint8_t { Query, Edit, Action };
enum class Exposure : std::uint8_t { McpTool, McpExec, Internal };
struct ErrorSpec {
    std::string code, when, hint;
};
struct Example {
    std::string title;
    json::Value args, result;
    std::string fixture;
};
struct Origin {
    enum class Type : std::uint8_t {
        Ui,
        Lua,
        Mcp,
        Rpc,
        Test,
        Internal,
        Agent
    } type = Type::Internal;
    std::string client, agent;
    // A continuous UI gesture (a fader drag): consecutive committed edits from
    // the same origin with the same non-empty gesture replace the head instead
    // of stacking — one undo step per gesture (session-model §6.2).
    std::string gesture = {};
};
using Warning = schema::Warning;

class Context {
  public:
    explicit Context(Origin origin = {}) : origin_(std::move(origin)) {}
    const Origin& origin() const { return origin_; }
    template <class S>
    void provide(S& service) {
        services_[std::type_index(typeid(S))] = {&service, !std::is_const_v<S>};
    }
    template <class S>
    bool has() const {
        return services_.contains(std::type_index(typeid(S)));
    }
    template <class S>
    S& service() const {
        auto it = services_.find(std::type_index(typeid(S)));
        assert(it != services_.end());
        assert(std::is_const_v<S> || it->second.mutableAccess);
        return *static_cast<S*>(const_cast<void*>(it->second.pointer));
    }
    void warn(std::string code, std::string message, std::string path = {}) {
        warnings_.push_back({std::move(code), std::move(message), std::move(path)});
    }
    const std::vector<Warning>& warnings() const { return warnings_; }
    // The arguments a command's resolve step settled on: what the dispatcher logs and
    // history records for the command the caller ran. Only the outermost invocation owns
    // them: a command that runs another through the registry with this same context
    // (CommandInfo::invoke) leaves them as they were (InvocationScope).
    void setResolvedArgs(json::Value value) { resolvedArgs_ = std::move(value); }
    const std::optional<json::Value>& resolvedArgs() const { return resolvedArgs_; }

    // Marks one CommandInfo::invoke on this context. A nested one (inside another
    // command's run) puts the resolved arguments back as it found them on the way out, on
    // every path, so its own resolve step cannot replace the caller's.
    class InvocationScope {
      public:
        explicit InvocationScope(Context& context)
            : context_(context), nested_(context.depth_++ > 0) {
            if (nested_) outer_ = context.resolvedArgs_;
        }
        ~InvocationScope() {
            --context_.depth_;
            if (nested_) context_.resolvedArgs_ = std::move(outer_);
        }
        InvocationScope(const InvocationScope&) = delete;
        InvocationScope& operator=(const InvocationScope&) = delete;

      private:
        Context& context_;
        bool nested_;
        std::optional<json::Value> outer_;
    };

  private:
    Origin origin_;
    struct ServiceEntry {
        const void* pointer;
        bool mutableAccess;
    };
    std::unordered_map<std::type_index, ServiceEntry> services_;
    std::vector<Warning> warnings_;
    std::optional<json::Value> resolvedArgs_;
    unsigned depth_ = 0;  // CommandInfo::invoke calls in progress on this context
};
struct NoArgs {};
// A reference is either a stable ID or a selector object. The host resolves
// selectors and supplies ambiguity errors with candidate IDs.
struct Ref {
    json::Value value;
};
class RefResolver {
  public:
    virtual ~RefResolver() = default;
    virtual Result<std::string> resolve(std::string_view kind, const Ref&) = 0;
};
schema::Schema<Ref> refSchema();
Result<std::string> resolveRef(Context&, std::string_view kind, const Ref&);

template <class Args, class Res>
struct Command {
    std::string name;
    int version = 1;
    Kind kind = Kind::Query;
    Exposure exposure = Exposure::McpExec;
    std::string summary, description;
    std::vector<std::string> tags;
    schema::Schema<Args> args;
    schema::Schema<Res> result;
    std::vector<ErrorSpec> errors;
    std::vector<Example> examples;
    // Optional host-side selector resolution after schema validation. Mutate Args to IDs.
    std::function<Status(Args&, Context&)> resolve;
    std::function<Result<Res>(const Args&, Context&)> run;
};

struct CommandInfo {
    // Index in registration order, set by Registry::add: what a crash report's command
    // breadcrumbs record in place of the name (ADR-0083, topic 5d).
    std::uint32_t id = 0;
    std::string name;
    int version = 1;
    Kind kind = Kind::Query;
    Exposure exposure = Exposure::McpExec;
    std::string summary, description;
    std::vector<std::string> tags;
    json::Value argsSchema, resultSchema;
    std::vector<ErrorSpec> errors;
    std::vector<Example> examples;
    std::function<Result<json::Value>(const json::Value&, Context&)> invoke;
    std::function<Status(const json::Value&)> validate;
    std::function<Status(const json::Value&)> validateResult;
};

class Registry {
  public:
    Registry();
    template <class Args, class Res>
    void add(Command<Args, Res> command) {
        assert(command.run);
        assert(command.exposure == Exposure::Internal || !command.examples.empty());
        auto args = command.args;
        auto result = command.result;
        CommandInfo info;
        info.name = std::move(command.name);
        info.version = command.version;
        info.kind = command.kind;
        info.exposure = command.exposure;
        info.summary = std::move(command.summary);
        info.description = std::move(command.description);
        info.tags = std::move(command.tags);
        info.argsSchema = args.jsonSchema();
        info.resultSchema = result.jsonSchema();
        info.errors = std::move(command.errors);
        info.examples = std::move(command.examples);
        info.validate = [args](const json::Value& value) { return args.validate(value); };
        info.validateResult = [result](const json::Value& value) { return result.validate(value); };
        info.invoke = [args, result, resolve = std::move(command.resolve),
                       run = std::move(command.run)](const json::Value& value,
                                                     Context& context) -> Result<json::Value> {
            Context::InvocationScope scope(context);
            std::vector<schema::Warning> warnings;
            auto decoded = args.decode(value, {}, &warnings);
            for (const auto& warning : warnings)
                context.warn(warning.code, warning.message, warning.path);
            if (!decoded) return decoded.error();
            if (resolve) {
                SUPPLE_TRY(resolve(*decoded, context));
                context.setResolvedArgs(args.encode(*decoded));
            }
            auto output = run(*decoded, context);
            if (!output) return output.error();
            return result.encode(*output);
        };
        addInfo(std::move(info));
    }
    const CommandInfo* find(std::string_view name, int version = 0) const;
    std::vector<const CommandInfo*> all() const;
    // The group of a command is the part of its name before the first '.' (the whole name for one
    // without a dot). describeGroup gives a group the one line the agent index shows for it, in
    // at most kMaxGroupSummary characters; a second line for a group replaces nothing and asserts.
    void describeGroup(std::string prefix, std::string summary);
    const std::map<std::string, std::string>& groups() const { return groups_; }
    // Every group that has a public (not Internal) command, by name: how many commands, and its
    // line (empty when none was declared; a test fails on that).
    struct GroupInfo {
        std::string name, summary;
        std::size_t commands = 0;
    };
    std::vector<GroupInfo> groupInfos() const;
    static constexpr std::size_t kMaxGroupSummary = 70;
    // The results of registry.groups and registry.list, computed here so that the commands and
    // the command line's `registry --groups` / `--group` give the same answer.
    json::Value groupsResult(std::string_view group = {}) const;
    json::Value listResult(std::string_view tag, std::string_view kind, std::string_view text,
                           std::string_view group) const;
    static std::string groupOf(std::string_view commandName);
    // The name of each command by id (a name once per version: the same name twice).
    std::vector<std::string_view> namesById() const;
    Error unknown(std::string_view name) const;
    json::Value exportJson() const;

  private:
    void addInfo(CommandInfo info);
    std::map<std::pair<std::string, int>, CommandInfo> commands_;
    std::map<std::string, std::string> groups_;
    std::uint32_t nextId_ = 0;
};

struct Invocation {
    std::string command;
    json::Value args = json::Value::object();
    Origin origin;
};
struct LogRecord {
    std::uint64_t seq = 0, time_ms = 0;
    Origin origin;
    std::string command;
    int version = 1;
    json::Value args;
    bool ok = false;
    std::string errorCode, resultDigest;
    std::uint64_t duration_us = 0;
};

// Workers update jobs through this thread-safe store. Events are queued until
// drainEvents() is called on the model thread, so Host::emit stays model-thread only.
//
// The longest `wait` blocks; a longer timeout_s is clamped to this, not
// rejected (the result then carries timeout_clamped_s).
inline constexpr double kMaxWaitSeconds = 30.0;

class JobManager {
  public:
    // Install before starting workers; outlives workers. Called after releasing
    // the store lock, only when an event was queued. Never dispatch inline.
    void setWaker(std::function<void()> wake) { wake_ = std::move(wake); }
    std::string create();
    bool cancelled(std::string_view id) const;
    std::shared_ptr<const std::atomic<bool>> cancellationFlag(std::string_view id) const;
    std::shared_ptr<std::atomic<double>> progressFlag(std::string_view id) const;
    Status progress(std::string_view id, double fraction, std::string message,
                    json::Value extra = json::Value::object());
    Status complete(std::string_view id, json::Value result);
    // Runs publish under the job lock only if the job is still running, then
    // makes the result visible. A concurrent cancel cannot publish afterwards.
    Status completeWith(std::string_view id, json::Value result,
                        const std::function<void()>& publish);
    // `result`, when given, is what the job did before it failed (a render
    // job's files written so far); the failed job's snapshot carries it.
    Status fail(std::string_view id, Error error, std::optional<json::Value> result = {});
    // A late completion may attach a result to an already cancelled job (for
    // example render.stale_target); this never changes its cancelled status.
    Status cancel(std::string_view id, std::optional<json::Value> result = {});
    Result<json::Value> status(std::string_view id) const;
    Result<json::Value> wait(std::string_view id, double timeoutSeconds);
    json::Value list() const;
    std::vector<std::pair<std::string, json::Value>> drainEvents();

  private:
    struct Job {
        std::string id;
        std::string status = "running";
        json::Value progress = json::Value::object();
        json::Value result;
        Error error;
        bool cancelRequested = false;
        std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<double>> fraction = std::make_shared<std::atomic<double>>(0.0);
        std::chrono::steady_clock::time_point lastProgressEvent{};
    };
    static json::Value snapshot(const Job& job);
    void prune();
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Job> jobs_;
    std::vector<std::string> order_;
    std::vector<std::pair<std::string, json::Value>> events_;
    std::uint64_t next_ = 1;
    std::function<void()> wake_;
};

class Dispatcher;
class Host {
  public:
    virtual ~Host() = default;
    virtual Context makeContext(const Origin&) = 0;
    virtual Result<std::uint64_t> beginTransaction(const Origin&, std::string_view label) = 0;
    virtual Status commitTransaction(std::uint64_t) = 0;
    virtual Status rollbackTransaction(std::uint64_t) = 0;
    // Snapshot the open transaction before an invocation. On failure the
    // dispatcher calls the returned restore function, even if Lua catches it.
    // Capture failure refuses the invocation. Restore failure poisons the
    // outer transaction, even if the caller catches the command error.
    virtual Result<std::function<Status()>> invocationSavepoint() {
        return std::function<Status()>{};
    }
    virtual void log(const LogRecord&) = 0;
    virtual void emit(std::string_view topic, json::Value payload) = 0;
    // Called on the model thread when a top-level command has finished (its
    // transaction committed or rolled back, its job events flushed) and, in a
    // script, after each command the script ran. Hosts that mirror bus events
    // to in-process subscribers (Lua `q.on`) hand them over here, so a
    // subscriber sees an event after the command that emitted it.
    virtual void commandFinished() {}
    virtual bool actionTransactional(std::string_view) const { return false; }
    // Called after a command ran successfully inside the current transaction
    // (before it commits), with the resolved arguments — hosts that replay
    // history (branch merges) record edits here. Not called for queries or
    // for the batch command itself (its steps are reported one by one).
    virtual void invoked(std::string_view /*command*/, int /*version*/, const json::Value& /*args*/,
                         Kind /*kind*/) {}
    virtual std::uint64_t clockMs() const;
    virtual void setupFixture(Dispatcher&, std::string_view) {}
    // Called by ~Dispatcher while its JobManager is still alive: stop and join
    // any worker threads that report into it.
    virtual void stopJobs() {}
    // The dispatcher owns the job store; hosts may cancel their project work.
    virtual void dispatcherAttached(Dispatcher*) {}
    // Any producer thread, after publication and outside bus locks. Wake the
    // model thread only; execution remains in flushJobEvents().
    virtual void workQueued() {}
};

class Dispatcher {
  public:
    Dispatcher(const Registry& registry, Host& host);
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Result<json::Value> execute(const Invocation& invocation);
    // Runs `body` inside one host transaction: every command `body` executes
    // through this dispatcher joins it (no nested transaction, and an Action
    // that is not transaction-safe is refused like a batch step), so the whole
    // body is one undo step and every invocation is recorded in order. An
    // error anywhere rolls the transaction back and passes through unchanged.
    // Called inside an open transaction it joins that one (the Lua `q.batch`
    // inside a script's own transaction).
    Result<json::Value> transact(std::string_view label, const Origin& origin,
                                 const std::function<Result<json::Value>()>& body);
    // Model-thread scoped admission, also applies to nested Lua/batch calls.
    using Admission = std::function<Status(const Invocation&)>;
    Result<json::Value> guarded(Admission admission,
                                const std::function<Result<json::Value>()>& body);
    const Registry& registry() const { return registry_; }
    JobManager& jobs() { return jobs_; }
    // Model-thread follow-up work (thread-safe to call): runs at the next
    // flushJobEvents(), i.e. right after the command that is executing, or on
    // the next loop tick when posted from a job's worker. A failure is emitted
    // as `bus.post_failed` {label, error}. Commands reach the dispatcher with
    // ctx.service<cmd::Dispatcher>().
    // `Edit` work changes the project on the caller's behalf (adding renders):
    // while a transact() transaction is open (a batch or script waiting in
    // job.wait) it stays queued and runs after that transaction closes, so it
    // never joins an unrelated undo step or rollback. `Service` work (job
    // completion) runs at any flush, so job.wait can see the job finish.
    enum class PostKind { Service, Edit };
    void post(std::string label, std::function<Status()> work, PostKind kind = PostKind::Service);
    // Runs posted work, then emits pending job events. Call it from the loop's
    // tick too: job events and follow-ups from workers wait for it.
    void flushJobEvents();

  private:
    Result<json::Value> dispatch(const Invocation&, bool inBatch, bool preResolved = false);
    Result<json::Value> batch(const json::Value&, const Origin&);
    const Registry& registry_;
    Host& host_;
    JobManager jobs_;
    std::uint64_t sequence_ = 0;
    std::vector<Admission> admissions_;
    // The process's first dispatcher records the commands it begins in the execution table
    // (ADR-0083, topic 5d); any other would write ids of a different registry into the ring.
    bool breadcrumbs_ = false;
    int transactionDepth_ = 0;  // > 0: commands join the open transact() transaction
    std::optional<Error> transactionFailure_;
    std::mutex postedMutex_;
    struct Posted {
        std::string label;
        std::function<Status()> work;
        PostKind kind;
    };
    std::vector<Posted> posted_;
};

using FixtureFactory = std::function<std::unique_ptr<Host>(std::string_view)>;
// Whether this build can provide a fixture (one that needs plugin editors,
// say); examples whose fixture it cannot are skipped. Every fixture when absent.
using FixtureAvailable = std::function<bool(std::string_view)>;
Status runExamples(const Registry&, const FixtureFactory&, const FixtureAvailable& = {});
// allTools includes every non-Internal command, regardless of MCP exposure.
// The agent's way into the registry, in three steps (spec command-bus.md, "Describe"): the
// groups with one line each, one group's (or a search's) rows, then one command by
// registry.describe. Both builders take a command's result, so the MCP server (which may only hold
// an RPC connection) and the command line shape them alike.
inline constexpr std::size_t kIndexRowCap = 60;
// registry.groups' result as {columns: [group, count, summary], groups: [[...]], hint}.
json::Value groupIndex(const json::Value& groupsResult);
// registry.list's result as {columns: [name, kind, summary], namespaces: {group: [[...]]}}:
// the public commands only, at most kIndexRowCap rows; a longer answer says so (truncated: true,
// with a hint to narrow the filter) and is never cut silently.
json::Value rowIndex(const json::Value& listResult);
// The error for a group that does not exist, its hint listing the groups.
Error unknownGroup(std::string_view group, const json::Value& groupsResult);
json::Value mcpToolList(const Registry&, bool allTools = false);
std::string markdownReference(const Registry&);
// LuaLS annotations of the generated bindings (ADR-0006): the `q` table a
// script sees — one class per command's arguments and result, one per command
// namespace, and the root. The script language itself is `libs/lua-bus`.
std::string luaStubs(const Registry&);

}  // namespace supple::cmd
