# Takes and comping

An item can hold several **takes**: passes recorded in a loop (one take per pass),
copies made with `take.add`, or renders. One take is *active*, the one you hear.
`take.list {item}` lists them; `take.activate {item, take|index}` hears another.

## Comping: playing one take here and another there

`take.comp` chooses the take for stretches of an item and crossfades the seams:

```json
{"item": "it_1", "spans": [
  {"from": 1.0, "to": 3.5, "index": 0},
  {"from": 5.0, "to": 6.0, "index": 2}]}
```

- `from` and `to` are beats, as `item.split`'s `at`. `index` is the take's place in
  the item's take order (0 is the first take in `take.list`); `take` takes a take id
  instead. What no span covers keeps the take it plays now.
- The item is split where the take changes. **Each piece keeps all the takes**, so any
  piece can be switched later with `take.activate {item: <piece>, index}`; that keeps
  the seams as they are.
- `crossfade` is seconds (default 0.01) and `crossfade_shape` is `cos` (equal power,
  right for different performances, the default) or `linear` (for the same
  material, phase-coherent). The crossfade is centred on the seam. It is shortened,
  with the warning `comp.crossfade_clamped`, where the pieces beside a seam are
  short. `crossfade: 0` is a hard cut.
- The answer lists the `pieces` in time order: `from` and `to` (the seams in beats),
  the `take` and `index` each plays, its `name`, and its `fade_in` and `fade_out`.

Call `take.comp` again on any piece (or give `items`, the pieces in any order) to move
seams or change spans. The crossfades are rebuilt for the whole comp, and the same
call twice changes nothing. `take.comp.get {item}` reads a comp you did not make: besides the pieces it gives the
`crossfade` (seconds) and `crossfade_shape` the comp has (its longest seam fade, so a seam
shortened by short pieces does not shrink it), and `take.comp` with those and the spans
rebuilds it unchanged, as long as `uniform` is true. A seam fade you set by hand (another
length or shape, or a hard cut among crossfaded seams) makes it false and lists the seams that
differ from the rest in `differing_seams`: `take.comp` would give every seam one length and
shape, so such a comp is not rebuilt byte for byte, and the listed seams are the ones that change.
Pieces of the same take beside each other (after a move, say) are joined into one
piece again, so moving a seam by a few samples gives exactly what the same spans give
from scratch, MIDI pieces too (a held note stays one note). A boundary where the
pieces differ (a gain, FX, edited notes) stays; between MIDI pieces of one take it
makes a held note sound again, and the warning `comp.midi_boundary` names the beats. A re-comp joins identical same-take pieces anywhere in the run, not only at a moved
seam: to switch one half of a piece later, `take.activate` after the re-comp, not
before. A copy made with `item.duplicate` and placed right after
the item is not part of its comp (its source does not continue), so comping one leaves
the other alone.

What to know:

- Audio only for crossfades. MIDI seams are always hard cuts, as `item.split`'s are:
  a note held across a seam ends with its piece. A take with stretch markers or
  envelopes, or an empty take, also turns the seams into hard cuts, and the
  warning `comp.crossfade_skipped` says so.
- A looped item is refused (`comp.loop`); an item with one take has nothing to comp
  (`comp.takes`); a locked item is refused (`item.locked`). An item that is edited
  together with others (an edit group, drums say) is comped together with them: the same
  spans, take indices and crossfade apply to the run of every other member, and `members` in the result gives each one's `track`
  and `pieces`.
  A track is a member when its items over the comp form a run that starts and ends where the
  comp does (however it is cut inside, whatever its crossfades, start offsets or play rates: a
  whole item beside a split one is comped to the spans too); a group track with nothing there
  (`no_item`) or only one-take items (`one_take`: a bass beside drum takes) is left alone and named in
  `skipped_tracks` with its reason. A track that cannot follow refuses the whole call and changes
  nothing: `comp.group_takes` (several takes, but fewer than a span's index), `comp.group_mismatch` (its items do not
  form a matching run; the message carries the reason, such as a looped item), `item.locked`; the error's
  `path` names the member (`/args/members/tr_2/items/it_5`, or `/args/members/tr_2` for the track), so read
  it from there, not from the message.
  (`comp.linked` is retired: it used to mean "set `bypass_groups`".) `bypass_groups: true` comps only the
  item you name. `take.comp.get` reads the members too, and `differing_members` lists a member
  that reads differently from the one you named (or does not line up), with `uniform` false.
  `differing_seams` lists the seams a `take.comp` with the reported `crossfade` and shape would
  rewrite.
- **Flatten a comp** with `item.consolidate` on its pieces: it renders the seam fades
  and layers the overlaps into one item. `item.heal` is for pieces of one source, not
  a comp.
- On the default Beats timebase a seam's overlap is stored in beats and its fades in
  seconds, so after `tempo.set` they disagree (a bump or a dip across the seam). Call
  `take.comp` again with the spans `take.comp.get` gives: it rebuilds both.
- One undo step. Nothing marks a comp in the file: its pieces are ordinary items.
  A comp is found from its pieces by their place and by holding the same takes, so
  renaming a take in one piece makes it a separate item (give `items` to be explicit,
  or `take.set {name}` it back).
