// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "bridge_diagnostics.h"
namespace supple::host::bridge {

BridgeOrdinals& bridgeOrdinals() noexcept {
    static BridgeOrdinals ordinals;
    return ordinals;
}

BridgeEnd classifyBridgeEnd(int why, bool hostKilled) noexcept {
    BridgeEnd end;
    end.status = why;
    if (hostKilled) {
        end.how = diagctx::BridgeHow::Host;
        return end;
    }
#if defined(_WIN32)
    end.how = (static_cast<std::uint32_t>(why) & 0xC0000000U) == 0xC0000000U
                  ? diagctx::BridgeHow::Exception
                  : diagctx::BridgeHow::Exit;
#else
    end.how = why > 0 ? diagctx::BridgeHow::Signal : diagctx::BridgeHow::Exit;
#endif
    return end;
}

}  // namespace supple::host::bridge
