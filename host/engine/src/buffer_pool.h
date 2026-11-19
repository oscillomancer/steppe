// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The compile's buffers, reused from retired plans (route E's compile
// profile, step 3; main thread only). Every compile allocated each stage's,
// node's and edge's buffers afresh and freed them when the plan retired:
// about 2.5 ms of a 10 ms compile at 192 tracks, with collectPlans' share.
// A plan retires when no executor can reach it again (collectPlans, as for
// its rings); its buffers then wait here for the next prepare(). Whatever is
// handed out is cleared as a fresh buffer would be, so nothing a reused
// buffer held can be heard.
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "supple/host/clap_host.h"

namespace supple::engine::detail {
struct Plan;
struct HeldVoice;

// Bytes of buffer capacity, by kind (BufferPool::heldBytes() and tests).
struct HeldBytes {
    std::size_t floats = 0, voices = 0, outputs = 0;
    std::size_t total() const { return floats + voices + outputs; }
};

class BufferPool {
  public:
    // While a Scope lives, this thread's zeroed(), reservedVoices() and
    // sizedOutputs() take from `pool` (null: they allocate). prepare() opens
    // one; the executors never see a pool.
    class Scope {
      public:
        explicit Scope(BufferPool* pool);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

      private:
        BufferPool* previous_;
    };

    BufferPool();
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Moves the buffers of a plan no executor can reach into the pool.
    void harvest(Plan& plan);
    // Keeps as many of each size as the last prepare() took, frees the rest.
    void trim();

    // Buffers handed out from the pool, and allocated because it had none
    // of the size (both counted only inside a Scope).
    std::uint64_t reused() const { return reused_; }
    std::uint64_t made() const { return made_; }
    // Buffer capacity the pool holds now (tests: trim()'s bound).
    HeldBytes heldBytes() const;
    // Tests: every harvested buffer is filled with loud garbage first, so a
    // reuse that fails to clear one is heard.
    bool poisonForTests = false;

    bool takeFloats(std::size_t n, std::vector<float>& into);
    bool takeVoices(std::size_t capacity, std::vector<HeldVoice>& into);
    bool takeOutputs(std::size_t n, std::vector<host::OutputEvent>& into);
    void countMade() { ++made_; }
    static BufferPool* current();

  private:
    void give(std::vector<float>& v);
    void give(std::vector<HeldVoice>& v);
    void give(std::vector<host::OutputEvent>& v);
    void closeScope();

    std::map<std::size_t, std::vector<std::vector<float>>> floats_;
    std::vector<std::vector<HeldVoice>> voices_;
    std::map<std::size_t, std::vector<std::vector<host::OutputEvent>>> outputs_;
    // What the open (and the last closed) prepare() took, by size.
    std::map<std::size_t, std::size_t> floatTakes_, lastFloatTakes_;
    std::map<std::size_t, std::size_t> outputTakes_, lastOutputTakes_;
    std::size_t voiceTakes_ = 0, lastVoiceTakes_ = 0;
    std::uint64_t reused_ = 0, made_ = 0;
};

// Every buffer of a plan that the pool takes back, by kind (harvest(), and
// tests that look at what a reuse hands out).
struct BufferVisitor {
    virtual void floats(std::vector<float>& v) = 0;
    virtual void voices(std::vector<HeldVoice>& v) = 0;
    virtual void outputs(std::vector<host::OutputEvent>& v) = 0;

  protected:
    ~BufferVisitor() = default;
};
void visitPooledBuffers(Plan& plan, BufferVisitor& visitor);

// `v` becomes `n` zeros: a pooled buffer of that size when the thread's
// scope has one, a new one otherwise.
void zeroed(std::vector<float>& v, std::size_t n);
// `v` becomes empty with room for `capacity` voices.
void reservedVoices(std::vector<HeldVoice>& v, std::size_t capacity);
// `v` becomes `n` default output events.
void sizedOutputs(std::vector<host::OutputEvent>& v, std::size_t n);
}  // namespace supple::engine::detail
