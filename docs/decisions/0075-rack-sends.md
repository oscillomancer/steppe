# ADR-0075: A rack's Out modules send to busses and masters

**Status:** Accepted, 2026-11-11. Amends ADR-0062 (rack.md §2.4 and §2.5:
an Out module may now feed a buss or master) and applies ADR-0019 §3 and
ADR-0020 §2 (linear signals join a summing strip after its decode).
Contracts: `docs/specs/rack.md` (§2.4, §2.5, §6), `docs/specs/engine.md`
(rack sends), `docs/specs/session-model.md` (schema 2.13), `docs/specs/gui.md`
§4.6, the `route.add`, `route.set`, `route.matrix`, `rack.list` and
`rack.get` commands.

## Context

A rack's audio leaves it through Out modules, and until now only as a
channel's input: the channel hears the Out before its items and inserts, and
its strip sends it on, encoded by its Console 9 Channel stage. Users want a
rack's Out to reach a reverb buss or a second master directly, the way a
strip's send does, without spending a channel on every output (a drum rack
with eight Outs would need eight channels just to sum them into two busses).

Busses and masters begin with a Console 9 Buss stage, a decode that expects
an encoded input (ADR-0019): every edge into a summing input is either a
strip's post-fader output or a send encoder, and nothing sits between an
encode and its decode. A rack's Out signal has not been through a channel,
so it is not encoded. Encoding it with a send encoder of its own would add a
Console 9 Channel stage the rack never asked for and, worse, would apply the
summing character to a signal that is not a channel's. Feeding it into the
decode unencoded would bend it by the decode curve.

ADR-0020 already has a linear entry into a summing strip: links into a
buss's main pair are added after its decode, linearly. Sends into a rack's
In module are linear too (rack.md §2.4).

## Decision

1. **A rack send is a route** `{from: {module: <Out>}, to: {track: <buss or
   master>}}` with no `midi`. There is no new object or field: the route
   end `module` exists since schema 2.7; what is new is that its destination
   may be a summing strip. An Out may feed its channel (the channel's
   `record.input`, as before) and have any number of rack sends; a send is
   in addition, never instead. An Out with no channel may still send. A
   channel still takes an Out only as its input; key and In-module
   destinations are refused.
2. **Level and mute only.** A rack send has `volume_db` (the send fader law,
   `faderGain`, at most +12.04 dB), `mute`, envelopes on `volume` and
   `mute`, and MIDI learn on `volume` and `mute`. It has no tap (the Out's
   signal is what it is), no pan, mono, polarity or channel map: those
   belong in the rack (or on the buss). Commands refuse them with
   `route.rack_send`; `env.add` refuses a `pan` envelope and
   `route.learn.set` other controls; the model's invariants refuse such
   files.
3. **After the decode, never encoded.** The send's level and mute run at
   the rack node, after the Out (a linear stage with the usual 10 ms ramps,
   like an In send's), and the destination adds the result to its main pair
   directly after its Console 9 Buss decode, where the auxiliary links of
   ADR-0020 join, before the pre-FX tap. Nothing touches it between the
   rack and that point except its compensation delay. It is independent of
   `project.console9`. The plan checker requires a rack send's destination
   to decode and a channel's Out edge not to. Non-finite samples become 0.
   Because the send's gain runs at its source, the edge's output is what
   the frontier rings carry when the destination is live and the rack
   renders ahead, as for any send.
4. **Mute and solo follow the home channel** (the rack's `track`): its
   mute, its mute envelope and its VCAs' mutes silence the send, as they
   silence the channel's own sends; its fader does not move it (the send is
   not after any fader). Solo treats the send as the home channel's send:
   a silenced home silences it, soloing the destination keeps the home
   (and so the send) audible, soloing the home keeps the destination. An
   isolated analysis tap ignores solo, as it does for strips.
5. **Latency.** The rack node's latency is that of its slowest routed Out,
   rack sends included (ADR-0064), and each rack send's edge delay aligns it
   with the destination's other inputs, as for an Out into a channel. Its
   envelopes are read where its gain acts: the rack's timeline shift plus
   the Out's latency.
6. **Cycles** are refused as for any route: the rack is a node of the
   routing graph, so a buss that reaches the rack (a send into its In, a
   MIDI send into a track its Notes read) cannot take its Out
   (`route.cycle`); `route.matrix` lists such pairs.
7. **Freeze.** Freezing the home channel prints what the channel hears,
   its rack included; it never covered the rack's other outputs. A rack
   send is not part of the print: the rack keeps running and its sends keep
   sounding live from it, following the home channel's mute. So that the
   print stays what the rack plays, a frozen home locks its racks: their
   modules, cables, plug-in modules' plug-ins (parameters and state), and
   the envelopes and automation items on them are dependencies of the
   freeze, and commands that would change them (`module.add`, `.set` of
   parameters, `.remove`, `cable.*`, `env.*`, `fx.param.set`, `rack.set
   track`, `rack.remove`) fail with `track.frozen` and a hint to unfreeze.
   A module's position, label and learn bindings and a cable's colour stay
   editable, as do the rack sends themselves (level, mute, envelopes). The
   rack is locked, not owned: its plug-ins keep their instances, and
   freezing does not reduce the rack's CPU, since the rack still runs for
   its sends (and for its other channels). Busses and masters do not
   freeze.
8. **Removal.** Removing an Out or its rack removes its rack sends.
   Removing the home channel moves the rack to another channel that hears
   it, as before; a buss that only takes rack sends is not a home, so a
   rack heard only through its sends is removed with them. `track.remove`
   says so in a warning (`track.rack_removed`, naming the rack and each of
   its sends, or `track.rack_moved`, naming the new home). Retyping a buss
   that takes a rack send to a channel is refused (`track.strip_type`,
   naming the send), and `route.set` does not turn a channel's input route
   into a send (`route.in_use`; add one with `route.add`).
9. **Schema 2.13.** Writers emit `supple.project/2.13` (2.11 is the clock
   output work, 2.12 one input per track). There is nothing to migrate:
   older files load unchanged. Older builds refuse a project that contains
   a rack send (their invariants allow an Out only into a channel).
10. **Routing window.** In Audio mode each channel's Out modules follow its
    row, one level in, named "<rack> » <Out>" with the rack colour (teal,
    as In-module columns). Only buss and master cells are open; a selected
    rack send shows its level and mute, with the tap, pan, channel and
    polarity controls disabled. `route.matrix` lists the rows (`type:
    rack_out`, with `rack` and `track`, and a `rule`) and their sends.
    `rack.list` and `rack.get` give each Out's `sends` beside the
    `channels` it is the input of.

## Consequences

- A rack's outputs can be mixed into busses and masters without a channel
  per output; Console 9's encode/decode pairing is untouched, since a rack
  send never enters a summing input before the decode.
- A rack send does not have the summing character a channel send has: it is
  added linearly. A user who wants that character routes the Out into a
  channel and sends from the channel.
- The mixer's strips do not show rack sends (they belong to no strip); the
  routing window and `route.list` do.
- Unlike a pre-fader strip send, which ignores its strip's mute (ADR-0019
  §3), a rack send has no tap and always follows its home channel's mute,
  like a post-fader send, but never its fader.

## Tests

`host/engine/tests/test_racks.cpp`: the buss's pre-FX tap with the send
equals the tap with the send muted plus the Out's signal at the send's level
(within 1e-6, Console 9 on, loud enough that a decode would bend it; it
fails by 0.54 with the send summed before the decode), with and without a
64-sample plug-in in the rack, at block sizes 512 and 37; the home channel
hears the Out unchanged; mute, a mute envelope, a VCA mute and solo; real
time equals offline in both executors with workers, for a playback rack, a
live rack, and a live buss with the rack rendered ahead (with and without
latency), with a volume envelope on the send. `host/engine/model/tests`:
round trip, home, learn and cycle refusal. `test_commands.cpp`: route.add,
route.set, env.add and route.learn.set refusals with hints, the routing
matrix rows, rack.list, save and reopen, removal with the home channel.
`daw/gui/tests/test_midi_editor.cpp`: the rows, open and blocked cells, the
controls, a double click that sends, removal.
