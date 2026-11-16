# ADR-0082: Audio takes and published media survive a crash

**Status:** Accepted, 2026-11-15. Amends ADR-0014 and extends ADR-0080's
journal and durability policy to audio. Contracts: `docs/specs/engine.md`
§4, `docs/specs/session-model.md` §5 and `docs/specs/core-analysis.md` §9.

## Context

The media audit found unaccounted prefixes after short writes, delayed error
reporting, WAVs flushed without a disk barrier, loop placement saved only at
stop, and streaming recording capped at RIFF's size limit despite ADR-0014's
RF64 decision. A saved project must not outlive the media it references.

## Decision

1. **Retain the physical prefix and report failures immediately.** A short
   write or failed buffered flush accounts for every complete frame physically
   written. The failed audio target stops writing; its header and stopped take
   describe that prefix. Other targets, playback and the engine continue.
   The recording writer latches its first write/sync error and wakes the model
   thread, which emits `record.error` without waiting for stop. The GUI displays
   the diagnostic in its status line. Stop remains a fallback if it races the
   notification; a failure is reported once per target. No file operations,
   error formatting or notification consumption occur on the audio thread.
   Opening, draining, header repair and closing recording files belong to the
   writer thread; the model joins it before committing takes.
2. **Bounded durable checkpoints.** The writer flushes each drain and syncs
   audio data and its header on the MIDI journal's shared five-second cadence,
   and at stop. The shared platform helper uses fsync on POSIX and
   FlushFileBuffers on Windows (the platform's stronger full-sync operation
   may be used where supported). File and directory creation/publication must
   be durable too. Copied/moved media and its containing directories are synced
   before publishing the referencing project and before removing originals.
   A sync failure is a target error, never an engine-fatal error. The bound
   assumes healthy storage making progress; blocked I/O cannot promise a
   deadline, and a failed checkpoint is reported rather than claimed durable.
   Windows uses only documented, unprivileged file FlushFileBuffers and
   MoveFileExW with MOVEFILE_WRITE_THROUGH for publication, matching project
   save. It never elevates or flushes a whole volume. These primitives do not
   establish POSIX-style parent/ancestor namespace durability after power loss;
   the five-second file-data bound does not imply that stronger namespace bound.
3. **An append-only audio placement journal.** Each take owns
   `<stem>.audio.jsonl`, schema `supple.audio-recording/1.0`, beside its WAV.
   Its header records track, sample rate, start, latency and the initial loop
   range; subsequent run and captured-frame records follow ADR-0080. Shared
   journal code validates clocks and torn tails and handles append/flush/sync.
   Audio data is flushed before publishing progress; durable checkpoints sync
   data/header before their placement journal. Recovery clips placement to the
   complete audio prefix and committed journal progress and uses normal stop's
   placement rules. Old WAV/sidecar recordings still recover. Journals appear
   through `project.recoverable` and retire only after successful save/autosave,
   with ADR-0080's ownership, Save As and discard rules. Final sidecars retain
   stopped takes' placement after journal retirement. Changing the loop range
   during a take retains ADR-0080's limitation: its initial loop is authoritative.
   MIDI journal failure keeps its existing warning/in-memory-take behaviour;
   an audio data or placement-journal failure stops that audio target.
4. **Promote streaming RIFF to RF64 in place.** New recordings reserve a
   28-byte `JUNK` payload for `ds64` at creation. Short recordings stay RIFF;
   crossing the 32-bit size limit promotes the reserved chunk and container
   to RF64, without shifting audio. Repair accepts old 44-byte RIFF headers
   and the reserved RIFF/RF64 layout. Decode, inspection and import accept RF64.
   Tests use a sparse file or an injected small threshold, never a real 4 GiB
   audio write. Existing recordings and project formats remain readable.

## Consequences

- The callback continues to publish only preallocated ring data and atomics.
- A process crash retains flushed audio and placement; a power cut retains
  the last completed durable checkpoint, nominally no more than five seconds
  behind. Unsupported barriers or failed writes are errors, not silent success.
- A take may be shorter after a storage failure; its written prefix stays
  available, and the user sees which target failed while others continue.
- Journal retirement does not delete audio payloads referenced by any saved
  project or branch. No filename-based orphan collector is introduced.
- Implementation proceeds in order: error/prefix handling, durability,
  placement journaling, then streaming RF64, one topic per commit.

*Clarified 2026-11-15 after review round 18:* normal-stop placement includes
the authoritative punch. Journal 1.1 headers store its initial value and a
changed value is committed atomically with progress; final sidecar 1.2 retains
it after retirement. Punch edits during recording keep ADR-0015's commit-time
rule: stop uses the final setting, recovery the last complete checkpoint's.
Old metadata without punch defaults to disabled, never the new app's setting.
