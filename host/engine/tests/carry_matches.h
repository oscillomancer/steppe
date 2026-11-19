// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Route E stage 6: the carries' lookups (findNode, carriedNode, matchInEdge,
// matchSend) pair exactly what the pairwise scans they replaced paired. The
// scans below are those loops, kept as the reference.
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "plan.h"
#include "rack_runtime.h"

namespace supple::engine::carry_test {
struct CarryMatches {
    std::uint64_t nodes = 0, edges = 0, sends = 0, live = 0;  // pairs found
    std::uint64_t wrong = 0;                                  // lookups that differ
    std::uint64_t otherFx = 0;  // key edges whose route now feeds another FX
};

// `from` -> `to` in each executor's classes: the callback's (-1), the
// renderer's (0), the live region's (1), the renderer's handoff into it
// (1 from 0).
inline void check(detail::Plan& from, detail::Plan& to, CarryMatches& out) {
    using detail::Edge;
    using detail::Node;
    for (const auto& [toClass, fromClass] :
         {std::pair{-1, -1}, std::pair{0, 0}, std::pair{1, 1}, std::pair{1, 0}}) {
        for (Node& n : to.nodes) {
            if (toClass >= 0 && n.live != (toClass == 1)) continue;
            Node* scanned = nullptr;
            for (Node& old : from.nodes)
                if (old.id == n.id) {
                    if (fromClass < 0 || old.live == (fromClass == 1)) scanned = &old;
                    break;
                }
            out.wrong += detail::carriedNode(from, n, fromClass) != scanned;
            if (!scanned) continue;
            ++out.nodes;
            out.live += toClass == 1;
            for (std::size_t ei : n.in) {
                const Edge& edge = to.edges[ei];
                std::size_t audio = detail::kNoEdge, send = detail::kNoEdge;
                for (std::size_t oi : scanned->in) {
                    const Edge& previous = from.edges[oi];
                    if (edge.route != previous.route || edge.tap != previous.tap ||
                        (edge.stage >= 0) != (previous.stage >= 0) ||
                        to.nodes[edge.from].id != from.nodes[previous.from].id)
                        continue;
                    if (edge.stage >= 0 &&
                        n.chain[static_cast<std::size_t>(edge.stage)].fx !=
                            scanned->chain[static_cast<std::size_t>(previous.stage)].fx) {
                        ++out.otherFx;
                        continue;
                    }
                    audio = oi;
                    break;
                }
                out.wrong += detail::matchInEdge(edge, n, from, *scanned) != audio;
                out.edges += audio != detail::kNoEdge;
                if (!edge.midi) continue;
                for (std::size_t oi : scanned->in)
                    if (detail::sameSend(to, edge, from, from.edges[oi])) {
                        send = oi;
                        break;
                    }
                out.wrong += detail::matchSend(to, edge, from, *scanned) != send;
                out.sends += send != detail::kNoEdge;
            }
        }
    }
    // carryLiveNotes and carryRateSources look the other way, from `from`.
    for (const Node& old : from.nodes) {
        const Node* scanned = nullptr;
        for (const Node& n : to.nodes)
            if (n.id == old.id) {
                scanned = &n;
                break;
            }
        out.wrong += detail::findNode(std::as_const(to), old.id) != scanned;
    }
}
}  // namespace supple::engine::carry_test
