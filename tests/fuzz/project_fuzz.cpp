// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/engine/app.h>
#include <supple/model/project_file.h>
#include <supple/model/schemas.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

namespace model = supple::model;

const supple::cmd::Registry& commands() {
    static const supple::cmd::Registry* registry = [] {
        auto* r = new supple::cmd::Registry;
        supple::engine::registerEngineCommands(*r);
        return r;
    }();
    return *registry;
}

// The rule project.open applies to every saved step (ADR-0076).
supple::Status replayable(const model::Invocation& step) {
    return supple::engine::EngineApp::replayableFromFile(commands(), step);
}

void checkHead(const model::VersionStore& store) {
    if (!model::checkInvariants(*store.head())) __builtin_trap();
}

}  // namespace

// The path project.open takes: the document, then its branches (ADR-0076);
// then what undo, redo and merges walk in what it restored.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > (1U << 20)) return 0;
    auto opened = model::parseProjectFile(
        std::string_view(reinterpret_cast<const char*>(data), size), nullptr, replayable);
    if (!opened) return 0;
    model::VersionStore& store = opened->store;
    for (const auto& branch : store.branches())
        if (!model::checkInvariants(*store.head(branch))) __builtin_trap();
    const std::string text = model::serializeProjectFile(store, true);
    auto again = model::parseProjectFile(text, nullptr, replayable);
    if (!again || again->branchesDropped || model::serializeProjectFile(again->store, true) != text)
        __builtin_trap();

    // Every branch: all the way down and back up. Each step lands on a saved
    // version (a walk off the branch would trap in at() or here).
    constexpr int kSteps = 4096;
    for (const auto& branch : store.branches()) {
        if (!store.switchBranch(branch)) continue;
        int steps = 0;
        while (steps++ < kSteps && store.undo()) checkHead(store);
        while (steps++ < 2 * kSteps && store.redo()) checkHead(store);
    }
    // Merges into main with a replayer that changes nothing: the history
    // walks (forks, replays held, remaps) are the real ones.
    if (!store.switchBranch("main")) __builtin_trap();
    const auto replay = [](const model::Session& base, const std::vector<model::Invocation>&,
                           std::uint64_t version) -> supple::Result<model::Replayed> {
        model::EditTxn txn(base, version);
        auto session = txn.commit();
        if (!session) return std::move(session).error();
        return model::Replayed{std::move(*session), {}, {}};
    };
    const auto schemas = [](std::string_view command) -> const supple::json::Value* {
        const auto* info = commands().find(command);
        return info ? &info->argsSchema : nullptr;
    };
    int merges = 0;
    for (const auto& branch : store.branches()) {
        if (branch == "main" || merges++ >= 8) continue;
        (void)store.merge(branch, "main", replay, schemas);
        checkHead(store);
    }
    return 0;
}
