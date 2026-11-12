# ADR-0070: Timecode and sync: MIDI clock, MTC, LTC and the chase tracker

**Status:** Accepted, 2026-11-10, for the library (`libs/sync`). Engine
integration, commands and the GUI follow in later changes and will amend
this ADR where they decide more. Builds on ADR-0045 and ADR-0046 (the master
playrate). Contract: `docs/specs/sync.md`.

## Context

Steppe is to send MIDI clock, Song Position Pointer and MIDI Time Code to
hardware, follow an external master by MIDI clock, MTC or SMPTE linear
timecode (LTC), and send LTC as audio. Following a master means driving the
live master playrate with pitch preserved (ADR-0045, ADR-0046), not editing
the tempo map: the project keeps its own tempo, and the transport runs as
fast as the master does. The pieces that do not depend on the engine are the
codecs, the timecode arithmetic and the control maths that turns an
external position into a playrate. They need to be exact, deterministic,
real-time safe and testable without a device, so they come first, as a
library the engine will call from its audio callback.

## Decision

1. **A GPL, DAW-only library: `libs/sync`.** Namespace `supple::sync`,
   headers under `supple/sync/`. It sits in `libs/` with `libs/midi` and
   `libs/timeline` because it depends on nothing in `host/` or `daw/` and is
   tested on its own. It is not one of ADR-0033's MIT libraries, and cannot
   be: it links libltc (LGPL, decision 9), so no plug-in, module or MIT
   library may link it. `tools/license-check`'s MIT list is unchanged, so
   the gate keeps first-party files there GPL-3.0-or-later.

2. **Timecode.** Rates 23.976, 24, 25, 29.97 drop-frame, 29.97 non-drop and
   30, each an exact fraction. A frame count (frames since 00:00:00:00)
   converts exactly to and from labels, drop-frame included (labels :00 and
   :01 skipped at every minute except each tenth), and wraps at 24 hours.
   Seconds are real time (count / fps), so 29.97 non-drop labels run 0.1%
   behind the clock. A project's timecode is a rate and a start label for
   timeline zero. Text is `HH:MM:SS:FF`, `;` before the frames in
   drop-frame, and parsing accepts `:`, `;`, `.` and `,`.

3. **MIDI clock out.** 24 clocks per quarter note, emitted at the exact
   (fractional) frame of every 1/24-beat crossing of a frame-to-beat map the
   engine implements from its tempo map and playrate clock, so tempo and
   playrate changes inside a block are exact. Both ends read clocks the
   same way: after Start the first clock is beat 0; after Continue it is
   the Song Position Pointer's position; each further clock adds 1/24 beat.
   - Starting at or before beat 0: Start, and the first clock at beat 0's
     frame (none during a count-in).
   - Starting after it: SPP of the next 16th at or after the position, then
     Continue at once; the first clock falls at that 16th's exact frame, so
     the receiver waits for it. A position on a 16th (within 1e-9 beats)
     starts there.
   - Locate or loop wrap while running: Stop, then as a start. While
     stopped: SPP alone, so receivers show the position.
   - Past 16383 sixteenths (1024 bars of 4/4) the SPP clamps; the clocks
     still count from the true position.
   Output is a caller-owned bounded event list; when it fills, the
   remaining clocks go out at the start of the next block.

4. **MIDI clock in.** Clocks, Start, Continue, Stop and SPP give running
   state and position (the latest clock's index, extrapolated by at most one
   clock). Tempo comes from a second-order delay-locked loop on clock
   arrival times (critically damped, bandwidth 0.3 Hz), whose bandwidth
   starts wide and narrows as clocks arrive (max(B, 1/(n·T)), a growing
   least-squares window in effect), so it locks in about half a second. A
   clock half a period off the prediction restarts the loop. Locked after
   24 steady clocks; lost after 0.25 s (or four periods) without one,
   which leaves the running state alone: the engine decides what a loss
   while running means. Clocks lost in a gap of up to 2 s while running are
   counted from the held period, so the position survives a dropout.

5. **MTC out.** Quarter frames at their exact frames, eight per two frames,
   pieces 0..7 forwards, each sequence starting on an even frame count and
   naming it; piece p goes out at F + p/4 frames. A full-frame SysEx
   (F0 7F 7F 01 01 hh mm ss ff F7, rate code in hh) names the frame playing
   at start, locate and loop wrap; quarter frames resume at the next even
   frame boundary. Stop sends nothing. 23.976 goes out with 24's code and
   29.97 non-drop with 30's, as MTC has no codes for them.

6. **MTC in.** Quarter frames assemble in either direction; each piece
   counts as F + p/4 frames, which accounts for the two frames a sequence
   takes to arrive (a forward sequence is known at piece 7, F + 1.75).
   Locked after two consistent sequences (about four frames); a skipped
   piece, a reversal or a mismatched sequence breaks the count; 150 ms
   without a quarter frame loses the signal. Full frames set the position
   and count as jumps. The rate comes from the code, from the project's
   rate when the user set one, or else, for the two ambiguous codes, from a
   least-squares fit of quarter-frame arrivals over two seconds of lock,
   decided once until the signal is lost. Until then such a source reads
   0.1% off (3.6 s at one hour), which the chase tracker sees as a jump.

7. **LTC.** Encoding and decoding use libltc (decision 9). The encoder
   stretches each LTC byte (a tenth of a frame) so it ends where the
   engine's frame-to-seconds map puts it, which follows playrate and tempo
   to the sample; it starts at the next frame boundary, restarts on a jump
   it was not told about, and is silent while stopped. Level −18 dBFS by
   default. All buffers are sized at creation, for speeds from 1/8 to 8;
   the per-block calls (`ltc_encoder_encode_byte`, the frame accessors,
   `ltc_frame_set_parity`, `ltc_decoder_write_float`, `ltc_decoder_read`)
   do not allocate. Labels are packed and unpacked here, because libltc's
   `ltc_frame_to_time` formats a time zone with `sprintf`, and because it
   sets the drop-frame flag for every 29.97. The decoder reports each
   frame with its sample positions, level and direction, the same lock and
   loss states as MTC (four consecutive frames; 0.2 s timeout), and the
   rate from the drop flag, the expected rate, or measured frame lengths
   (23.976 and 29.97 non-drop after two seconds, as for MTC).

8. **One chase tracker for all three.** Receivers hand it observations,
   (device frame, external position); once per block the engine passes the
   local position and its nominal speed (units per second at playrate 1:
   bpm/60 for beats, 1 for timecode seconds), and gets a playrate, the
   error in seconds, a relocate flag and a lock state.
   - Estimator: a second-order tracking loop on the observations,
     ẋ = v + 2ζω·r, v̇ = ω²·r, ζ = 0.707, bandwidth 0.5 Hz, integrated per
     observation with bounded gains; the first 0.1 s of observations seed
     the speed. A residual beyond 0.5 s of external time (or a receiver's
     jump) re-seeds the position and keeps the speed.
   - Control: rate = v/nominal + e/Tp + ∫e/(Tp·Ti), e the position error in
     seconds, Tp = 2 s, Ti = 8 s; the integral runs only within 20 ms of
     lock, so it trims bias without winding up; the correction stays within
     ±10% of the external speed; the rate within the playrate's 0.25..4.
     The crossover is near 0.5 rad/s. The engine's playrate path (a ramp of
     at most 300 ms and the 150 ms low-pass of ADR-0046, plus render-ahead,
     about 0.5 s in all) costs about 15° of phase margin, leaving some 60°;
     the tests run the loop through a modelled 0.3–0.5 s delay and the
     low-pass.
   - Relocation with hysteresis: when the error stays above 0.25 s for
     0.1 s, relocate once (to `predict(frame + seek latency)`), feeding the
     external speed forward meanwhile; re-arm when the error falls below
     0.05 s, or retry after 1 s if the relocate did not land. A relocate
     that lands 0.05–0.25 s off is chased, not repeated.
   - Lock: within 2 ms for 0.5 s once the estimate has settled (1 s);
     unlocked beyond 10 ms.
   - Deterministic: the outputs depend only on the sequence of calls.

9. **libltc, an approved LGPL exception.** libltc 1.3.2 (LGPL-3.0-or-later,
   © Robin Gareus) was approved by the maintainer on 2026-11-10, on the
   terms of LAME's (ADR-0055 §8): vendored unmodified (the `src/` library
   sources of the release tarball, SHA-256
   `0a6d42cd6c21e925a27fa560dc45ac80057d275f23342102825909c02d3b1249`),
   linked statically into the GPL-3.0-or-later DAW only, whose source offer
   covers it, never into `plugins/`, `modules/` or the MIT libraries.
   `tools/license-check` allows it by name, DAW-only; AGENTS.md rule 1 and
   `THIRD_PARTY.md` list it.

10. **Not decided here:** how the engine schedules the generators' events to
    ports (per-port offsets, the WinMM output thread), which inputs feed the
    receivers, the transport policy on signal loss, commands and project
    fields, and the GUI.

## Consequences

- The engine gets sample-exact clock and MTC output and an LTC signal from
  the same frame-to-position maps it already has, and one control law for
  all three inputs. Its integration must split blocks at transport events
  (start, stop, locate, loop wrap) and call the generators for each piece.
- Positions before beat 0 send no clocks, and positions between 16ths start
  the receiver at the next 16th: up to a 16th of silence from a slave.
- Unless the project's rate is set, 23.976 and 29.97 non-drop sources need
  two seconds before their positions are right.
- A third-party LGPL component enters the DAW build on every platform.

## Alternatives considered

- **Chasing by editing the tempo map.** Rejected: the project's tempo would
  change under the user, and recorded material would not stay where it was
  recorded; ADR-0045/0046 already provide a smoothed, pitch-preserving
  playrate.
- **Writing an LTC codec.** Rejected for now: libltc is small, mature and
  approved; the biphase decoder's edge cases (level changes, reverse,
  varispeed) are where a new codec would spend its time.
- **A Kalman filter for the estimator.** A fixed-gain second-order loop is
  equivalent in steady state for this noise, simpler to bound and to make
  deterministic.
- **Sending clocks during a count-in, or rounding a start down to the
  previous 16th.** Rejected: both put the slave at a position the master is
  not at.
