// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "buffer_pool.h"

#include <algorithm>
#include <utility>

#include "plan.h"
#include "rack_runtime.h"

namespace supple::engine::detail {
namespace {
thread_local BufferPool* tCurrent = nullptr;  // the open Scope's pool, this thread only

template <class Map, class Value>
bool take(Map& lists, std::size_t n, std::vector<Value>& into) {
    const auto it = lists.find(n);
    if (it == lists.end() || it->second.empty()) return false;
    into = std::move(it->second.back());
    it->second.pop_back();
    return true;
}

template <class Map, class Takes>
void trimTo(Map& lists, const Takes& takes) {
    for (auto it = lists.begin(); it != lists.end();) {
        const auto wanted = takes.find(it->first);
        const std::size_t keep = wanted == takes.end() ? 0 : wanted->second;
        if (it->second.size() > keep) it->second.resize(keep);
        it = it->second.empty() ? lists.erase(it) : std::next(it);
    }
}
}  // namespace

BufferPool::Scope::Scope(BufferPool* pool) : previous_(tCurrent) { tCurrent = pool; }

BufferPool::Scope::~Scope() {
    if (tCurrent) tCurrent->closeScope();
    tCurrent = previous_;
}

BufferPool::BufferPool() = default;
BufferPool::~BufferPool() = default;

BufferPool* BufferPool::current() { return tCurrent; }

void BufferPool::closeScope() {
    lastFloatTakes_ = std::exchange(floatTakes_, {});
    lastOutputTakes_ = std::exchange(outputTakes_, {});
    lastVoiceTakes_ = std::exchange(voiceTakes_, 0);
}

bool BufferPool::takeFloats(std::size_t n, std::vector<float>& into) {
    ++floatTakes_[n];
    if (!take(floats_, n, into)) return false;
    ++reused_;
    return true;
}

bool BufferPool::takeVoices(std::size_t capacity, std::vector<HeldVoice>& into) {
    ++voiceTakes_;
    for (auto it = voices_.rbegin(); it != voices_.rend(); ++it)
        if (it->capacity() >= capacity) {
            into = std::move(*it);
            voices_.erase(std::next(it).base());
            ++reused_;
            return true;
        }
    return false;
}

bool BufferPool::takeOutputs(std::size_t n, std::vector<host::OutputEvent>& into) {
    ++outputTakes_[n];
    if (!take(outputs_, n, into)) return false;
    ++reused_;
    return true;
}

void BufferPool::give(std::vector<float>& v) {
    if (v.empty()) return;
    if (poisonForTests) std::fill(v.begin(), v.end(), 0.5f);
    floats_[v.size()].push_back(std::move(v));
    v = {};
}

void BufferPool::give(std::vector<HeldVoice>& v) {
    if (v.capacity() == 0) return;
    if (poisonForTests) {
        HeldVoice junk;
        junk.noteId = 7;
        junk.key = 61;
        v.assign(std::min<std::size_t>(v.capacity(), 16), junk);
    }
    voices_.push_back(std::move(v));
    v = {};
}

void BufferPool::give(std::vector<host::OutputEvent>& v) {
    if (v.empty()) return;
    if (poisonForTests) {
        host::OutputEvent junk;
        junk.kind = host::OutputEvent::Kind::NoteOn;
        std::fill(v.begin(), v.end(), junk);
    }
    outputs_[v.size()].push_back(std::move(v));
    v = {};
}

void visitPooledBuffers(Plan& p, BufferVisitor& v) {
    const auto stereo = [&](Stereo& s) {
        v.floats(s.l);
        v.floats(s.r);
    };
    const auto delay = [&](Delay& d) {
        v.floats(d.buf[0]);
        v.floats(d.buf[1]);
    };
    for (Edge& e : p.edges) {
        stereo(e.encoded);
        delay(e.delay);
        stereo(e.tmp);
        for (auto& link : e.auxLinks) v.floats(link.delay.buf);
        stereo(e.frontierBuf);
        v.floats(e.auxTmp);
    }
    for (Node& n : p.nodes) {
        forEachStage(n, [&](Stage& st) {
            delay(st.dry);
            stereo(st.out);
            stereo(st.dryBuf);
            stereo(st.key);
            stereo(st.pinBuf);
            delay(st.align);
            stereo(st.groupIn);
            stereo(st.groupOut);
            delay(st.osPad);
            for (int c = 0; c < 2; ++c) {
                v.floats(st.osIn[c]);
                v.floats(st.osOut[c]);
                v.floats(st.osKey[c]);
            }
            for (auto& buffer : st.auxOut) v.floats(buffer);
            for (auto& d : st.chanDelay) v.floats(d.buf);
            v.voices(st.voices);
            v.voices(st.pendingVoices);
            v.outputs(st.outStorage);
        });
        for (TakeChain& tc : n.takeChains) stereo(tc.buf);
        stereo(n.inputBuf);
        stereo(n.sum);
        stereo(n.preFx);
        stereo(n.preFader);
        stereo(n.post);
        stereo(n.postKey);
        stereo(n.items);
        stereo(n.mix);
        for (auto& buffer : n.aux) v.floats(buffer);
        v.floats(n.auxPostGain);
        for (Stereo& out : n.rackOuts) stereo(out);
        for (Stereo& in : n.rackIns) stereo(in);
        // A rack's plug-in bindings: only when no one else holds the rack.
        if (n.rack && n.rack.use_count() == 1)
            for (PluginBinding& b : n.rack->plugins) stereo(b.io);
    }
    for (RecordTap& tap : p.recordTaps) stereo(tap.pulled);
}

void BufferPool::harvest(Plan& p) {
    struct Give final : BufferVisitor {
        BufferPool& pool;
        explicit Give(BufferPool& to) : pool(to) {}
        void floats(std::vector<float>& v) override { pool.give(v); }
        void voices(std::vector<HeldVoice>& v) override { pool.give(v); }
        void outputs(std::vector<host::OutputEvent>& v) override { pool.give(v); }
    } give(*this);
    visitPooledBuffers(p, give);
}

void BufferPool::trim() {
    trimTo(floats_, lastFloatTakes_);
    trimTo(outputs_, lastOutputTakes_);
    if (voices_.size() > lastVoiceTakes_) voices_.resize(lastVoiceTakes_);
}

std::size_t BufferPool::heldBytes() const {
    std::size_t bytes = 0;
    for (const auto& [size, list] : floats_)
        for (const auto& v : list) bytes += v.capacity() * sizeof(float);
    for (const auto& v : voices_) bytes += v.capacity() * sizeof(HeldVoice);
    for (const auto& [size, list] : outputs_)
        for (const auto& v : list) bytes += v.capacity() * sizeof(host::OutputEvent);
    return bytes;
}

void zeroed(std::vector<float>& v, std::size_t n) {
    BufferPool* pool = BufferPool::current();
    if (n > 0 && pool && pool->takeFloats(n, v)) {
        std::fill(v.begin(), v.end(), 0.0f);
        return;
    }
    if (n > 0 && pool) pool->countMade();
    v.assign(n, 0.0f);
}

void reservedVoices(std::vector<HeldVoice>& v, std::size_t capacity) {
    BufferPool* pool = BufferPool::current();
    if (pool && pool->takeVoices(capacity, v)) {
        v.clear();
        return;
    }
    if (pool) pool->countMade();
    v.clear();
    v.reserve(capacity);
}

void sizedOutputs(std::vector<host::OutputEvent>& v, std::size_t n) {
    BufferPool* pool = BufferPool::current();
    if (n > 0 && pool && pool->takeOutputs(n, v)) {
        std::fill(v.begin(), v.end(), host::OutputEvent{});
        return;
    }
    if (n > 0 && pool) pool->countMade();
    v.assign(n, host::OutputEvent{});
}
}  // namespace supple::engine::detail
