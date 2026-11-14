// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The engine's DSP script commands (ADR-0032): dsp.write, dsp.check, dsp.get,
// dsp.list, dsp.test and dsp.export over EngineApp's script library
// (host/dsp-commands).
#include "../../dsp-commands/dsp_export.h"
#include "../../dsp-commands/dsp_preview.h"
#include "../../dsp-commands/dsp_test.h"
#include "supple/engine/app.h"

namespace supple::dsp_commands {
template <>
struct TestTraits<engine::EngineApp> {
    // At the project's rate, in the sandbox's blocks.
    static sandbox::RenderSettings settings(const engine::EngineApp& app) {
        sandbox::RenderSettings r;
        r.sampleRate = app.head().project->sampleRate;
        return r;
    }
    static host::HostIdentity identity(const engine::EngineApp& app) {
        return app.options().identity;
    }
    static std::filesystem::path outputDir(const engine::EngineApp& app) {
        return app.rendersDir();
    }
};
}  // namespace supple::dsp_commands

namespace supple::engine {

void registerDspCommands(cmd::Registry& registry) {
    dsp_commands::registerDspCommands<EngineApp>(registry, cmd::Exposure::McpExec);
    dsp_commands::registerDspTest<EngineApp>(registry, "com.suppleaudio.test.gain",
                                             cmd::Exposure::McpExec);
    dsp_commands::registerDspExport<EngineApp>(registry, cmd::Exposure::McpExec);
    dsp_commands::registerDspPreview<EngineApp>(registry, cmd::Exposure::McpExec);
}

}  // namespace supple::engine
