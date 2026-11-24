# Steppe manual

This manual covers using Steppe once it is installed ([Installing
Steppe](installing.md)) or built ([Building Steppe](building.md)): starting
the app, audio devices, projects, plugins, and connecting an AI agent.

The workflow follows REAPER's, so most of the arrange view, mixer, FX chains,
routing and actions list will feel familiar from it. The actions list includes
the public commands and GUI actions (`Shift+/` or `Ctrl+Shift+P`,
`Cmd+Shift+P` on macOS).

## Contents

- [Starting Steppe](#starting-steppe)
- [Audio](#audio)
- [Projects](#projects)
- [Tracks and items](#tracks-and-items)
- [Editing items](#editing-items)
- [The song's key and chords](#the-songs-key-and-chords)
- [The MIDI editor](#the-midi-editor)
- [Plugins](#plugins)
- [Mixing](#mixing)
- [Routing](#routing)
- [Automation](#automation)
- [Modulation](#modulation)
- [Timing audio with hits and stretch markers](#timing-audio-with-hits-and-stretch-markers)
- [Racks](#racks)
- [MIDI controllers](#midi-controllers)
- [Sync](#sync)
- [The clip launcher](#the-clip-launcher)
- [Rendering](#rendering)
- [Agents (MCP)](#agents-mcp)

## Starting Steppe

Start Steppe from the application menu, the Start menu or Finder, or from a
terminal:

```sh
steppe                 # a new project
steppe song.json       # open one
steppe --help          # every command-line option
```

Command names follow the product name. In a build tree, the app is
`build/<preset>/daw/gui/steppe`.

A new project is empty. To begin,
drop audio or MIDI files onto the arrangement (each makes a track), or click
**+ track** at the bottom left. The **Browser** (press **B**)
lists plug-ins, saved FX chains and files to add.

The last of the transport buttons sets how the view follows the playhead.
Click it to go round the three modes: by page (the default: when the playhead
leaves the view, the view jumps ahead), off (the view stays where you put it),
and smooth (while playing, the view scrolls with the playhead held in the
middle). The button is lit while it is off or smooth. The mode also sets where
the mouse wheel zooms: on the playhead while the view follows it, under the
mouse pointer when following is off. The actions list has the same choices
under "View: … playhead", for binding keys. Each project remembers its mode.

## Audio

Steppe plays through JACK on Linux (PipeWire's JACK support works) and
through the system's audio device on macOS (Core Audio) and Windows (WASAPI
by default, or ASIO).

**Edit > Audio device…** (also a row in **Edit > Preferences…**) chooses the
audio system, the output and input devices, the sample rate and the buffer
size. Steppe remembers the choice and starts with it next time. Each change
restarts the running engine at once.

**Edit > Preferences…** holds the settings that apply to every project, in
sections: **Audio**, **Recording and import**, **Plug-ins** and **Editing**.
Each takes effect when you change it. The low-level tuning (how many threads
process audio, locking memory, keeping CPUs out of deep idle) is under
**Advanced**: click **Show** beside it. You do not need those to get started.
Locking memory and the CPU idle setting work on Linux only, and are listed
only there.
In a low window the settings scroll under the dialog's heading.
If your preferences file was written by a newer version of Steppe, this
version does not read it: it uses its defaults and says so at the top of the
dialog. The first time you change a setting it keeps the newer file beside the
new one, as `preferences.json.supple.preferences-<version>.bak` (a later copy
gets a number, `….2.bak`; none is ever overwritten). The newer version reads
only `preferences.json`: to use its settings there again, rename the `.bak`
file back to `preferences.json`.

- **Linux:** the rate and buffer size are requests to PipeWire, which shares
  the graph with other apps. It changes the rate only while no other client
  is playing, and only to a rate it allows. Choose **Force** to hold both on
  the whole graph while Steppe's engine runs. Other apps then run at Steppe's
  rate and buffer size too, and the device can keep them after Steppe quits.
- **macOS:** a chosen rate switches the device's own rate, which it keeps
  after Steppe quits. The buffer size is
  the device's I/O size. Allow microphone access when macOS asks, so that
  recording can use your input.
- **Windows:** WASAPI shares the device with other apps and converts other
  rates. ASIO uses one device for input and output, and its driver may round
  the buffer size.

On Linux, while the engine runs, Steppe keeps its own memory out of
transparent huge pages, except where a plug-in asks for them. With the
system set to `always`, the kernel otherwise merges memory in the
background, and audio can stop for a fraction of a second while it does. This
applies to Steppe and its plug-in bridges only: it changes no system setting,
and ends when Steppe stops the engine. macOS and Windows do not merge memory
this way, and nothing changes there.

If the device is missing or busy when Steppe starts, the app still opens. The
Audio device dialog then opens and says why. Choose another device, or
**Restart audio** once the device is ready. The dialog's status lines show
what is running: the backend, rate, buffer size, device period and output
latency.

The right end of the transport bar shows what the device is running at, for
example **48 kHz · 256 samples**. It stays quiet while all is well. If
something goes wrong, an amber note appears beside it and stays until the
audio engine restarts:

- **Audio stopped**: no device is running. Open **Edit > Audio device…**.
- **dropouts**: the device asked for audio and did not get it in time. You
  heard a click or a gap. A larger buffer size helps.
- **render gaps**: Steppe had not finished rendering a stretch of the
  project when it was due. A larger buffer size or fewer heavy plug-ins
  helps.
- **plug-in errors**: a plug-in refused to start. Its track plays without
  it until it starts.

## Projects

A project is a JSON file. Its media lives in the `media/` folder beside it.
Project and FX chain files accept oversampling factors 1, 2, 4 or 8. If a
hand-edited file uses another factor, loading reports the field and allowed values.

A project file keeps every branch, not just the one you are on: an agent's
branch you have not merged yet is still there after quitting and reopening,
with each of its steps (**View > History and command log**, a tab of the
side panel), and it can be undone step by step or
merged into main as before. The file opens on the branch that was current when
it was saved. Main's own undo history is not kept: after reopening, undo on
main starts from the saved state. (Agents can keep it with
`project.save {"history": true}`.) Autosaves keep branches too.

If a program that does not know about branches (an older Steppe, a script)
changed the file's main project, the saved branches no longer fit it: Steppe
opens main alone and says so, and the first time you save over that file it
keeps a copy of the old one beside it as `<file>.pre-branches.bak`.

Until a project is saved, its recordings go to a folder of its own under the
data folder (below), in `unsaved/<start time>-<pid>/media/rec`. The first
save moves them into `media/` beside the project file; a file that could not be removed from
the unsaved folder afterwards stays there, and the save warns with its path (the project
already uses the copy; delete the old one by hand). On Windows that is a file something still
has open: the waveform display of a recording you saved at once, a render or freeze still
running, or another program. After you quit and start Steppe again,
that file is in the earlier run's unsaved folder, which the list of unsaved projects to recover
shows; recover it or delete it there. While a recording runs, a
project cannot be saved for the first time, or saved as into another
folder: stop recording first, and its files move or are copied then.
Saving in place works while recording. Quitting, or starting
another project, without saving deletes that folder after the app has asked.

If an audio recording cannot be written, the status line shows the failed
file and error immediately. That target keeps every complete sample frame
written so far and stops writing; other armed targets and playback continue.
Stop and save the retained takes, then check free space and storage before
starting another recording.

An unsaved project is autosaved into that folder every minute while it
changes, like a saved one. If Steppe stops without you saving or discarding
(a crash, a power cut), the folder stays, with its autosaves and recordings.
Audio and MIDI recordings are kept as they are played. With storage keeping
up, a program crash loses about the writer's last pass; a power cut retains
the last completed file checkpoint, at most about five seconds behind. Storage
failures are shown immediately for audio, with the written prefix kept.
On Windows file data is flushed without elevation, but a power cut can still
lose newly created file or folder names; the file checkpoint does not promise
that stronger namespace guarantee. A failed save that reports the project may
already exist keeps both original and copied media; check storage before retrying.
The next time Steppe starts without a project to open, it lists what was left
behind: each with its name, when it was last autosaved, how many tracks and
recordings it has, and its size. **Recover** opens it (after asking about any
unsaved changes in the current project), still unsaved: save it to keep it.
**Delete** removes it for good, after asking once more. **Later** closes the
list and keeps everything; File > Recover unsaved work… shows it again. A
Steppe that is still running keeps its own folder to itself, so two copies
of the app never offer each other's work.

The same list appears when you open a saved project that Steppe autosaved
after its last save, or whose recordings were not added because Steppe
stopped while recording. **Open autosave** replaces what you opened with
those later changes; the file itself is not changed until you save.
**Put back** adds the recordings where they were recorded, as one step you
can undo.

New, Open and Quit (including File > Quit, Cmd+Q on macOS and closing the main
window) first stop an active recording and add its takes to the
project, then ask whether to save the changes. Cancel keeps the recorded takes;
Undo removes that recording. If stopping fails, the error is shown and the
requested action does not continue. If an agent starts another recording or
edits the current project while that question or its Save As dialog is open,
the app stops the new recording and asks again before saving or leaving.
A changed branch also requires a new decision.

If another project replaces the current one while the save-changes question is
open, any button cancels the old request with a notice, leaving the replacement
project untouched. File and folder dialog results are also ignored with a notice
if the project was replaced; open the dialog again. If edits arrive
while Open is waiting, Steppe asks about unsaved changes again before opening.

**File > Import…** (Ctrl+I) brings in audio files (WAV, FLAC, MP3) and MIDI
files (`.mid`) at the playhead: one file onto the selected track, several onto
new tracks named after them. Dropping files on the arrange view does the same.
A MIDI file's notes keep their beats and play at the project's tempo (the file's
own tempo is not applied). Imported MIDI
plays through its channel's instrument, so add one to a new track to hear it.

Renders go to `~/Documents/steppe-media` unless you choose another folder in
the render dialog. The folder you choose is remembered and is shared by all
projects (see *Rendering*).

### Where Steppe keeps its files

| | Settings (preferences, menus, controls, appearance) | Data (unsaved projects, DSP scripts, rack modules, render presets) |
| --- | --- | --- |
| Linux | `~/.config/steppe` (`$XDG_CONFIG_HOME`) | `~/.local/share/steppe` (`$XDG_DATA_HOME`) |
| macOS | `~/Library/Application Support/steppe` | `~/Library/Application Support/steppe` |
| Windows | `%APPDATA%\steppe` | `%APPDATA%\steppe` |

Uninstalling Steppe leaves these folders and your projects in place.

### Local crash reports (staged, ADR-0083)

Crash diagnostics are being added separately from autosave and recording recovery.
Reports remain on this machine under the app’s local data folder/diagnostics,
limited to 20 reports or 50 MB, oldest removed. They exclude your project, audio,
MIDI and command arguments. Steppe never uploads them; your OS crash reporting
continues under your own OS settings. After an unexpected close, the next start shows a
banner under the transport that says so (naming the plug-in the report points at, when it
points at one, or saying no report was written); Dismiss hides it for good. It is about the
app's own earlier closes only: a headless engine service that an agent started has its own line
on standard error (naming itself, "Steppe engine"), and dismissing one never dismisses the other.
It is a note, not a question, and project recovery is offered separately. An agent connected over MCP can list, read,
acknowledge, settle and delete these reports (`crash.list`, `crash.read`, `crash.acknowledge`,
`crash.settle`, `crash.delete`, `crash.clear`); a report names plug-ins and commands, never project
content. Before opening a project, or recovering one (`project.plugins` with `session` or `autosave`),
`project.plugins` says which of its plug-ins a recent
report points at, and `plugin.override` loads a plug-in on a bridge or not at all for the
run (`bridge`, `disable`), so the project can be opened safely; `project.open` and `project.recover` warn of such a plug-in
and load it normally unless one of those was set first. **Help > Crash reports…** lists
what earlier runs left, newest first (a bar marks a report you have not marked as seen), and shows one in plain
words: what stopped Steppe, which plug-in the report points at and how far to trust that, and, folded away, the
plug-ins that were loaded, the threads, the last commands and what the report could not record. You can copy a text
summary, save a copy of the report file wherever you choose (it is never sent anywhere), mark a report as seen, delete one report or all of them (each asks for a second click), or open the folder.
The banner's "Show report" button opens the same window on that report. 
**When a plug-in may have closed Steppe.** If a recent report points at a plug-in your project uses, Steppe
asks before it opens the project (File > Open, Recover, or starting from a template) how to load that plug-in
this time: in a separate process, so that a new fault stays there (the suggested choice); disabled this time, so that
its tracks pass audio through and the project keeps the plug-in; or normally, which also marks the reports as dealt
with. Choose one and press Continue; nothing is saved in your project by this choice. A plug-in that comes with
Steppe offers the last two only, since a fault in one is Steppe's own. Cancel, or Escape, cancels the open. Tick
"Remember my choice" to have Steppe apply the same answer the next time without asking; it is kept for that exact
version of the plug-in, so an updated plug-in is asked about again, and a remembered "normally" is dropped if that
plug-in closes Steppe again. Remembered choices are saved in the preferences file.

Two limits of that question. It looks at the project as saved, at the latest step of each branch: a plug-in
that only an older step of the history uses is not asked about, and loads normally if you undo back to that
step. And a project template is checked and then loaded by its name, so a template file that is changed
between the question and the load is loaded without the question.

A plug-in you disabled is marked in the mixer (its slot keeps its outline, its name goes dim and
an amber bar runs down the slot's left edge; a plug-in that failed, because its separate process closed or
stopped answering, has the same slot with a red bar; and a track's "fx" turns amber while its chain holds a
disabled or a failed plug-in, in the mixer and in the track list), in the FX window ("disabled after an unexpected close · audio passes through") and in
a rack (its module says "disabled"). Right-click it in the mixer or a rack, or press Enable in the FX window, to
load it again. The choice lasts until you quit Steppe. If a failed plug-in left a report, its row in the FX window has
a Show report button beside Restart.

#### Stopping Steppe from a script or a service

Steppe runs as two processes, a supervisor that you start and a worker that does the
work. Stop it the ordinary way: close the window, or send the program `SIGTERM` or Ctrl-C
(on Windows, Ctrl-C in its console, or close the window). Ending only the process you
started, with `kill -9`, Task Manager or `TerminateProcess`, stops the program too: the
worker notices that its supervisor vanished and shuts down as if it had been sent
`SIGTERM`, ending hard if that has not finished within ten seconds. It then frees its
control socket, so starting Steppe again works. A window closed this way does not ask
about unsaved changes, as with `SIGTERM`. If Steppe's supervisor itself crashes, the worker
keeps running without crash capture.

## Tracks and items

### Colours and names

Every new channel and buss gets a colour: the one the fewest of your tracks
use, starting with red. The colour marks the track's left edge, the top of
its mixer strip and every item on the track, so you can find a part in the arrangement
by its colour. To change it, right-click the track in the track list or the
mixer and choose **Color…**, then one of eight colours or **None**. A track
without a colour has blue items. Projects made before colours were given
automatically keep their tracks as they were.

An item shows its name on a bar across its top. The name is its take's. An
item made from an audio file that has no name of its own shows the file's
name. Where an item fades in or out, the faded part is shaded and the fade's
curve is drawn over it. A selected item is brighter and has an outline in the
accent colour.

### A track's buttons

A track has the same buttons in the same order in the track list and on its
mixer strip, each a small rounded box. The ring arms the track for
recording; armed, its box is red. The speaker sets input monitoring: a faint
outline when off, solid when on, green when automatic (you hear the input
while the track is armed). **M** mutes it and **S** solos it. **Ø** inverts
polarity and **mono** sums left and right. A button that is on is filled
with its colour; one that is off is an outline.

Rest the pointer on a button and the status line under the arrangement says
what it is called, and the keys that do the same when it has any: "Mute",
"Transport: play/stop (Space)". This works for a track's buttons, fader and
pan in the track list and the mixer, the transport bar and the toolbar.

Right-click a track's fader, pan or button, in the track list or the mixer,
for its menu. It is the same in both places: **MIDI learn** binds the control
to a controller (see MIDI controllers), **Modulate** offers an LFO, a
random source, a macro or a follower of another track's level and sets it
up in one step (greyed on a switch or a frozen track). When a modulation
there is idle, say because nothing is cabled into its source or the
envelope that carries it is off, the control shows no live movement and a
greyed line under **Modulate** says why. **Show lane** opens
its automation
lane under the track (a new lane starts at the control's value; **Hide
lane** puts it away, even when a modulation is what shows it), and on a fader or pan **Reset** sends it home, as a
double-click does: 0 dB, or the centre.

In the track list the first four are at the right of the track's name, at
any row height. Under them is a line with **Ø**, **mono**, the fader and
the pan, and then a line with the input, **fx**, **+**, **env** and **out**.
A new project's rows are just tall enough for all three lines; drag a row's
bottom edge to make it taller, or lower, which hides the lines from the
bottom up.
On a mixer strip, from the top: the name, **fx** and **+** over the
plug-ins, the sends, the input and **out**, **Ø** and **mono**, the pan,
then arm, monitor, **M** and **S** directly above the fader.

A buss has no arm, monitor or input, and their places stay empty. A buss is
marked by a small sign of lines joining before its name, and a master by the
same sign ending at a bar; a channel has no mark.

### Levels in the mixer

Each mixer strip has a meter beside its fader. The numbers along the meter
are decibels: **0** is full scale, and 12, 24, 36 and 48 are that
far under it. Each number has a line that runs across the meter's two bars,
so you read a level where a bar crosses a line. A bar is green low down
and turns yellow, then orange as the level rises, and red from just under
0 dB up. The number under the fader is the fader's own setting. The number under
the meter is the highest peak the track has reached since you last cleared
it; it turns red when the peak went over 0 dB, as the light at the top of
the meter does. Click that number or the meter to clear both.

### The side panel

A panel at the right of the window holds the **Inspector**, the **Browser**
and the **History**. It stays closed until you ask for it:

- **View > Inspector**, or the **I** key, opens it on the Inspector. The
  Inspector names what is selected: an item with its track, a track with its
  kind, or how many of each. Press **I** again to close the panel.
  With one item selected it also shows the item's settings, and you can
  change them there: its **Name**, where it **starts** (bar.beat, for
  example `3.2`, or `3.2.5` for half a beat after it), its **length** in
  beats, whether it **follows** the beats
  or the clock when the tempo changes, its **gain**, its **fades** in
  seconds, and **Mute**, **Loop** and **Lock**. Type a value and press
  Enter, or click elsewhere; Escape leaves it as it was. Each change is one
  step for Undo. A locked item cannot be changed until you turn Lock off.
  An audio item also has **Speed** (`0.5` is half speed), **Pitch** in
  semitones and **Keep pitch**, which holds the pitch when the speed
  changes. An item with several takes lists them; click one to hear it.
  With several items selected, the Inspector shows the settings they can
  share. A field is empty and says "mixed" when they differ; what you type
  goes to all of them as one step for Undo.
  With a track selected it shows the track's **Name**, **Colour** (click a
  swatch, or the empty square for none; from the keyboard, Tab to the row,
  Left and Right to move, Enter to pick), **Volume** and **Pan** (`-30` is
  30% left), and **Notes**, a few lines of your own about the track: Enter
  starts a new line, and the notes are kept when you click elsewhere or
  press Ctrl+Enter. With several tracks selected, a change goes to all of
  them.
- **View > Browser**, or the **B** key, opens it on the Browser: the list of
  your plug-ins, with the stock ones and your favourites first. Type to
  search by name, maker or kind ("reverb"). Select one or more tracks, then
  press Enter, double-click a plug-in or click **Add** to put it on them.
  **Add** lights up once a plug-in is chosen: click one, use the arrow
  keys, or search (the first one found is chosen).
  The Browser stays open, so you can add another. You can also drag a
  plug-in from the list onto a track, in the arrangement or on its mixer
  strip; the track it will land on is outlined. **Favourite** keeps a
  plug-in at the top; **Rescan** looks for newly installed ones.
  **Chains**, beside **Plug-ins** at the top of the Browser, lists the FX
  chains you saved with **Save chain…** in a track's FX window. Add one or
  drag it onto a track the same way; it is added after the plug-ins the
  track already has.
  **Files** lists the audio and MIDI files of a folder, starting in your
  project's folder. Double-click a folder to go into it and click **Up** to
  go back. Double-click a file, or select it and click **Add**, to put it
  on the selected track at the playhead (on a new track when none is
  selected); or drag it to the spot in the arrangement where you want it.
  Drop it under the last track to make a new one.
  To hear an audio file before you add it, select it and click **Play**;
  the button reads **Stop** while it plays. It plays on your first two
  outputs and is not part of the project: it does not pass your tracks or
  the master, and is never in a render or a recording. It stops when you
  click **Stop**, choose another file, add it or leave the page. A long
  file takes a moment before it sounds, and only its start is played: the
  first ten minutes at 48 kHz, less at higher sample rates (two and a half
  minutes at 192 kHz). MIDI files cannot be played this way.
  **Media** lists the audio your project already holds, each file with its
  format and length, so you can use a recording or an imported file again
  without looking for it on disk. Add it or drag it as you would a file;
  the project keeps one copy however often you use it. A file that is no
  longer where the project expects it is marked "missing"; the page looks
  for the files when it opens and when the project's audio changes.
  **Play** hears one here too.
- **View > History and command log**, or **Ctrl+H**, opens it on the History:
  the versions you can step back to, the branches, and every command with who
  ran it.

Click a tab to switch, **Close** to put the panel away, and drag the panel's
left edge to make it wider or narrower. Steppe remembers per project whether
the panel was open, which tab it showed, how wide it was and which kind the
Browser showed.

## Editing items

An item is a piece of audio or MIDI on a track. Every edit here is one step
of undo: while you drag you see a ghost of the result, and the project
changes when you let go. Where this section says Ctrl, use Cmd on macOS.

### Selecting

Click an item to select it. Shift-click adds an item to the selection and
Ctrl-click adds or removes one. A drag from empty space, or a drag with the
right button from anywhere, draws a selection rectangle: it takes the items
and the automation points inside it, on any track, and with Shift or Ctrl
held it adds them to what is selected. `Ctrl+A` selects every item.

### Moving, copying and trimming

- **Move:** drag an item by its middle, in time or onto another track.
  Ctrl-drag leaves the item where it is and drags a copy.
- **Trim:** drag an item's left or right end. Every selected item is trimmed
  by the same amount.
- **Fade:** drag one of the item's top corners inwards. Ctrl-click a fade to
  step through its shapes.
- **Slip:** Alt-drag an item to move its audio under it while the item stays
  where it is.
- **Stretch:** Alt-drag an audio item's right end, and the audio plays
  faster or slower to fit the new length.

**Crossfades** make themselves. Move, copy or trim an item so that it
overlaps another on the same track, and the earlier item fades out while
the later one fades in over the overlap, as part of the same step of undo.
An item placed wholly inside another gets no fades. To change a crossfade,
drag the fade corners of the two items as you would any fade.

Drags land on the grid, which follows the zoom: as fine as a sixteenth of a
beat when you are zoomed far in, as coarse as 32 beats when zoomed far out.
Hold Shift to place something off the grid.

The pointer shows what a drag would do. Over an item's left or right end,
or its top corners where the fades are, it becomes a pair of sideways
arrows: drag there to trim the item or draw a fade. Over the middle it
stays an arrow: drag to move the item. The same sideways arrows show at
the ends of the loop range in the ruler and of a note in the piano roll,
and up-and-down arrows at a track row's lower edge and at the top edge of
the mixer and the MIDI editor, which set their height. While you carry
something to drop it (a plug-in out of the browser, a mixer strip, a track
row, a module for a rack), the pointer is a four-way arrow where letting go
would put it, and a "not allowed" sign over a place that will not take it.
The routing matrix shows the same sign over a cell that cannot be connected.

To drag something further than the window shows, hold it at the edge of
the arrangement: the view scrolls that way, faster the further out you
hold it, and stops when you bring the pointer back in. This works for
items, their ends and fades, automation points, a selection rectangle,
and the loop range, markers and regions in the ruler. An item being moved
also scrolls the tracks up and down. The piano roll does the same for notes:
hold a note or a selection rectangle at an edge and the roll scrolls in
time, or up and down the keys. So does the mixer, sideways, for a strip, a
plug-in or an output you are dragging to a strip that is out of view, and
the rack window, for modules, a cable or a selection rectangle. A plug-in
held at the top or bottom of its own strip's list scrolls that list, when
the chain is longer than the list shows.

### Splitting, copying and deleting

| Key | What it does to the selected items |
| --- | --- |
| `S` | Splits them at the cursor (the playhead) |
| `Delete` or `Alt+C` | Deletes them |
| `Ctrl+C`, `Ctrl+X` | Copies or cuts them |
| `Ctrl+V` | Pastes at the cursor |
| `Ctrl+D` | Duplicates each one after itself |

A right-click on an item opens its menu, with the same actions (**Paste**
there pastes where you clicked) and with **Consolidate items**, **Heal
splits**, which joins the pieces of a split audio item again, **Split item
at prior zero crossing**, **Mix to new track** and **Edit MIDI**. The
**Item** menu in the menu bar has the rest: exporting an item to a file, its
timebase, warping and its takes.

### Takes

An item that holds several takes shows their number on its name bar; click
the number to hear the next take. **Item > Show all takes** lays the takes
out as lanes inside the item: click a lane to hear that take, or drag across
part of a lane to use that take for that stretch only. Ctrl-drag from below
the name bar pulls a take out into an item of its own.

Dragging across a lane makes a **comp**: the item is split where the take
changes, and each seam between two takes gets a short crossfade (10 ms at
first). A line with a small notch at its foot marks each seam. Drag the line
to move the seam, or drag the top of it to make the crossfade longer or
shorter; Ctrl+click there switches between an equal-power and a linear
crossfade. One crossfade length and shape serves every seam of a comp, and
the inspector shows it as **Crossfade** for a selected piece: type 0 for hard
cuts. Changing the crossfade there or at a seam's top sets every seam. Moving
a seam, or comping again elsewhere, leaves the other seams as they are, even
one an agent shortened or reshaped by hand. Clicking a lane of one piece
switches just that piece. To make a comp
one plain item again, select its pieces and use **Consolidate items**. On a
track grouped for editing, every track of the group follows: a drag, a click
on a lane or the take number comps each of them the same way. If one of them
has been changed on its own since, its seam lines are drawn broken and the
inspector's **Group** row says which track it differs from. Other comp edits
leave its own seams as they are; dragging the top of a seam (the crossfade) on
the group, or setting the **Crossfade** in the inspector, brings it back in
step. A track of the group with a single take there (a bass beside
the drum takes) is left as it is. If a track cannot follow (a looped or locked
item, or one that starts or ends elsewhere), the status line says so and nothing
changes: hold **Alt** while you drag, click or pick a take to comp this track
alone. An item whose Loop is on is comped with hard cuts (no
crossfades) until you turn its Loop off.

**Recording over an item** adds a take to it instead of laying a second item on top: arm the
track, put the playhead (or a punch-in) inside the item and record, and the item keeps its place
and gains a take, which you hear where you recorded it: punch into bar 5 of an eight-bar item and
bars 1 to 4 and 6 to 8 go on playing what they did, with the new take in bar 5 (the item is split
into pieces there, with a short crossfade, as **take.comp** makes them; one undo puts it back). A
pass over the whole item is heard whole. The older takes stay and **Show all takes** (or
`take.comp`) lets you pick between them. A pass that starts before the item, or into a locked or
looped item, still makes a new item, and the recording says so. If the pass runs past the item's
end, the rest stays in the take, hidden; lengthen the item to hear it. If the item was a comp
(several pieces of one set of takes), every piece gets the take. If you record only some of the
tracks of an edit group, the others do not get a take, and the recording says so: arm every track
of the group to keep their takes level. A pass inside a comp leaves the comp's other seams exactly as they were, ones you set by hand
included. Agents turn this off with
`record.set {stack_takes: false}` (every pass is then laid as items, as in older versions);
`record.get` reads it. In the app it is **Edit > Preferences > Recording and import > Record over
an item**: **New take** (the default) or **Layer on top**, kept with your other preferences; an
agent's change shows there and lasts until Steppe quits. When a pass became a new item instead of a
take, or other tracks of an edit group were left without a take, the status line says so when the
recording stops, whether you stopped it, an agent did or the audio engine did.

### Changing what the mouse does

**Edit > Mouse modifiers…** lists these gestures (dragging an item, dragging
its end, clicking it, pressing a fade, dragging in empty space, the wheel)
and lets you choose what each does with Shift, Ctrl and Alt held.

## The song's key and chords

A song can say what key it is in, and where the key changes. Key changes
show in the ruler's tempo row, under the tempo, beside the time signatures,
as "F major" or "D dorian". To set one, right-click in that half of the row
and choose **Key change here…**, then type a note name and a scale and press
Enter: "Eb major", "C# minor", "F dorian". The scales are major, minor,
harmonic minor, melodic minor, the modes (dorian, phrygian, lydian,
mixolydian, locrian), major and minor pentatonic, blues and chromatic.
Double-click a key change to edit it, drag it to move it, or right-click it
to delete it.

Chords have a row of their own in the ruler, under the regions. It appears
when the song has its first chord; to start one, choose **View > Chord row**,
then double-click in the row and type a chord as you would write it in
notation, such as "Am7", "Bbmaj7", "F#7#9" or "C/E", and press Enter. Press
Tab instead of Enter to go on to the next bar. Each chord lasts until the
next one. Double-click a chord's name to change it, drag it to move it (with
Alt, to copy it), and right-click it to delete it. Right-click inside a chord and choose **No chord
from here** to end it early.

Agents read and write the same thing. `context.get` returns the key and chords
of a range as a lead sheet (one line per bar, such as `3: {Fm7}:h {G7}:h`),
`context.write` writes one back as a diff over the bars it names, `context.at`
says what holds at a bar, and `key.set` and `chord.set` change one point.
The key also decides how notes are spelled: the same pitch reads as `Bb` in F
major and `A#` in D major in the MIDI editor and in `midi.get`. Under a chord a
chord tone is spelled by its degree instead (the third of E is `G#`, never `Ab`;
the seventh of C7 is `Bb`; a diminished seventh on C has `Bbb`), in `midi.get`
and in the MIDI editor alike (the roll's note names, the notation panel and the
event list, whose pitch cell takes `Bbb4` or `F##4` typed in). The scale-aware
editing commands (`midi.edit` with `snap_to_scale`, `transpose_degrees` and
`fit_to_chord`) read the key and chord at each note. A **section** is a region
the song names (a verse, a chorus): `context.at` names the one that holds,
and `scene.insert` and `arrangement.build` add one for each scene they lay out
(see The clip launcher).

## The MIDI editor

Double-click a MIDI item to open it in the MIDI editor, a panel along the
bottom of the window (`Ctrl+E` opens the selected item; `Alt+E` shows or
hides the panel). To start a new part, double-click empty space on a
channel: that makes an empty MIDI item a bar long and opens it. Drag the
panel's top edge to set its height; a double-click there puts it back.

The panel's main part is the **piano roll**: time runs left to right, the
keys are at the left, and each note is a bar drawn in its item's colour,
fuller the higher its velocity. The strip along its top numbers the song's
bars, as the arrangement's ruler does, so you can see where in the song the
part plays, and under the numbers it names the song's chords over the part
(see [The song's key and chords](#the-songs-key-and-chords)). Double-click a
chord there to change it, or double-click between chords to add one, as in
the ruler. When the song
has a key, the rows of the notes outside its scale are drawn darker, and
note names are spelled for the key and chord where each note starts
(B-flat, not A-sharp, in F major; G-sharp under an E chord). With
no key the rows follow the black keys, as on a piano.

The button in the header after **Quantize** names the key where the part
starts. Click it to light it: dragging notes up and down then moves them
along the scale, a step at a time, and so do the `Up` and `Down` keys. A
note you drag goes to the scale note nearest the pointer, in the key that
holds where you drop it. Click the button again to move by semitones.

### Notes

- **Add:** double-click empty space. The note is as long as the last one you
  clicked or sized, or one grid step.
- **Delete:** double-click the note, or select notes and press `Delete`.
- **Move:** drag a note, in time or to another pitch. Selected notes move
  together.
- **Change its length:** drag either end of a note.
- **Select:** click a note; Shift-click adds or removes one; drag from empty
  space for a selection rectangle. `Ctrl+A` selects every note and `Escape`
  none.

| Key | What it does to the selected notes |
| --- | --- |
| `Up`, `Down` | Moves them a semitone (with Shift, an octave); a step of the scale while the key button is lit |
| `Left`, `Right` | Moves them a grid step earlier or later |
| `Q` | Quantizes them to the grid (every note when none is selected) |
| `Ctrl+C`, `Ctrl+X`, `Ctrl+V` | Copies, cuts, and pastes at the cursor |
| `Ctrl+D` | Duplicates them |

A right-click opens a menu with Delete, Copy, Cut, Paste (where you clicked)
and Duplicate.

### The grid

The buttons in the panel's header choose the grid: **1/4**, **1/8**,
**1/16**, **1/32** and **1/8T** (eighth-note triplets). **Snap** turns
snapping to it on and off, and **Quantize** does what `Q` does.

### Velocity and controllers

Under the roll, the **Vel** lane has a stem for each note: drag near a
note's start to set its velocity, and the other selected notes change by
the same amount. Below it is a controller lane. Drag in it to draw values,
one for each grid step; drag with the right button, or with Alt, to erase.
The **Lanes** menu in the header picks the lanes: Velocity, and the
controller (mod wheel, volume, pan, expression or sustain). Pick the shown
one again to hide it.

On a launcher clip, the Lanes menu also offers the clip's take envelopes
(Take volume, pan and mute, once the take FX hold an instrument) and the
parameters of each effect in its take FX (**parameters…**). Their lanes
appear under the controller lane in clip beats, from the clip's start,
and work like the timeline's envelope lanes: click to add a point, click a
point to select it (Ctrl+click for more), drag to move the selected points
together (Shift to leave the grid), Alt+click to remove one, and Delete to
remove the selected. Right-drag (or Ctrl-drag from empty space) to draw a
box around points and select them. Ctrl+C, Ctrl+X and Ctrl+V copy, cut and
paste the points of the lane you clicked last. Right-click a lane for its
menu: insert a point, select all, copy, cut, paste at the pointer, delete,
give the selected points a shape (linear, square, slow, fast start, fast
end, bezier) or hide the lane. **hide** at the lane's left
hides it too; the envelope keeps playing while hidden.

### Moving around

The wheel zooms in time around the pointer. With Shift it scrolls in time,
with Ctrl it scrolls up and down the keys, and with Alt it makes the keys
and rows taller or lower. **Edit > Mouse modifiers…** can change these.

### Notation and the event list

**Notation** in the header shows the item as text beside the roll, in the
notation Steppe's agents write. The two are one thing: edit a note in the
roll and the text changes; type in the text and, after a short pause, the
roll follows. A mistake in the text is reported under it, with its place,
and changes nothing until it is put right. If the item's MIDI was changed
somewhere else while you typed, the panel says so and `Ctrl+Enter` applies
your text over that change.

**List** swaps the roll for a list of the item's events, notes and
controller values alike, one to a row:

| Column | What it shows | To change it, type |
| --- | --- | --- |
| Position | Where the event is, as bar.beat.tick (960 ticks to a beat) | `3.2.480` |
| Type | "Note", or "CC" and the controller's number | (not editable) |
| Length | A note's length, as beats.ticks | `1.0`, `0.480` |
| Pitch / CC | A note's pitch by name, or the controller's number | `C#4` or `61`; `7` |
| Vel / value | A note's velocity (1 to 127) or the controller's value (0 to 127) | `96` |
| Channel | The MIDI channel | (not editable) |

Double-click a cell to edit it, then press Enter (Escape leaves it as it
was). A value the list cannot use is refused, and the line under the list
says what form it wants. **+ Note** under the list adds a note, and `Delete`
removes the selected rows.

## Plugins

Steppe hosts CLAP and VST3 plugins. It looks for them in this order:

1. its own stock plugins, installed privately with the app (in a build tree,
   `build/<preset>/plugins/clap`);
2. `CLAP_PATH`, then the standard CLAP folders: user before system on Linux
   (`~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap`) and macOS
   (`~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP`);
   system before user on Windows (`%COMMONPROGRAMFILES%\CLAP`,
   `%LOCALAPPDATA%\Programs\Common\CLAP`);
3. `VST3_PATH`, then the standard VST3 folders in the same platform order.

The first copy of a plugin id wins. An older copy of a stock plugin installed
for other hosts is listed as shadowed and never loaded. `--plugin-path P`
(repeatable) replaces the whole list.

To add an effect or instrument, use the **+** beside **fx** on a track or
mixer strip, or double-click the empty part of a mixer strip's plug-in
list. The
**fx** button opens the
track's FX chain, including when it is empty. In the mixer, right-click a
plugin to rename, duplicate or delete it; a duplicate keeps the plugin's
current settings and goes right after it. Alt-click (Option-click on macOS)
also deletes a plugin. A bypassed plugin's name is dimmed and struck through
on its strip; one taken offline is dimmed inside a dashed outline.
The chain window's **<** and **>** at the top left step to the previous or
next track, bus or master in mixer order, and clicking its title lists them
all to pick from.
In the chain window, Ctrl-click (Cmd-click on macOS) toggles a row in the
selection, including the active row. Effects moved to another track leave
that window’s selection.

Each row in the chain window says where its plugin runs: in-process (inside
Steppe), shared bridge (in a helper process shared with other plugins) or
isolated (in a process of its own). A plugin that crashes in a bridge
process goes silent and can be restarted, and Steppe keeps playing; one
that crashes in-process takes Steppe down with it. If a bridged plugin
crashes or stops responding (for a minute) while you export, freeze or
apply FX, that operation fails and names the plugin rather than leaving it
silent in the result; try again, or bypass or replace the plugin. **Edit > Preferences… >
Third-party plug-ins run** sets where plugins you have not set yourself
run: in the app (the default), in a shared bridge process, or each in its
own process. Those rows say "(default)". The choice applies as plugins
load (adding one, opening a project, restarting one); Steppe's own plugins
always run in the app.

On Linux, while a plugin in a bridge process plays live, for example on a
monitored track, Steppe keeps the CPUs out of deep idle, as **Edit > Preferences… > Advanced > Keep CPUs out of deep idle** does whenever
the engine runs. A bridged plugin waits for its bridge to wake, and a CPU in
deep idle wakes late. This is a request Steppe's process holds, not a system
setting: it ends when no bridged plugin plays live, when the engine stops or
when Steppe quits, and nothing changes after. While held, the computer may
use more power and run warmer. Many systems let only administrators make the
request; the preference's note then says it was refused, audio plays
without it, and the preference's caution line says how to allow it. With the
preference on, the request is held whenever the engine runs. macOS and
Windows have no such per-process request (Windows' power plan is
system-wide, and Steppe leaves it alone); there Steppe's audio threads and
its bridges' ask for the system's audio class instead, as they always have.

## Mixing

Beyond the fader and the buttons on every track (see Tracks and items),
these are the tools for a mix that has grown: where a sound sits between
the speakers, tracks that move together, and channels frozen to audio.

### Panning

Right-click a channel's or buss's pan, in the track list or the mixer. Under
its MIDI learn, lane and reset entries, the menu chooses how it pans:

- **Pan mode: pan** (the default) balances a stereo track and pans a mono
  one.
- **Pan mode: dual pan** splits the control in two: the left bar places
  the left input and the right bar the right input. Narrow, widen, move or
  swap the image without losing either side.
- **Pan mode: pan and width** also splits it: the left bar places the
  image, the right bar sets its width (100% as recorded, 0% mono, below 0
  the sides swapped). Panning out narrows the image so both sides stay
  heard.

Double-click a bar to send it home (centre, hard left or right in dual pan,
100% width). With the control focused, Up and Down choose a bar and Left and
Right move it.

The same menu sets the **pan law**: how much a centred mono signal is cut.
**Project** follows the project's law, set in **Project** in the menu bar;
the others (0, −2.5, −3, −4.5 and −6 dB) give the track its own. 0 dB, the
default, never cuts or boosts, so a centred signal keeps its level through
any number of busses. A cut law applies at every track a centred signal
passes. **Gain compensation (boost pans)** keeps the centre at 0 dB and
boosts the sides instead, which can clip a loud recording. A master's pan is
a plain balance, with no mode or law.

The track's envelope menu offers **Width**, **Pan left**,
**Pan right** and **Gain compensation** lanes on channels and busses. Width
and the two positions move continuously between −100 and 100%; compensation
switches on at 0.5 and overrides the current law's boost while its lane is
active. Both bars record automation in touch, latch and write modes. Pan's
lane keeps driving the position and has no effect in dual mode.

The pan group link and selection drags move all pan positions by relative
offsets, clamped at the ends. Mode and law choices copy to the other members;
masters are skipped for these extra controls.

### Track groups and VCA faders

Select the tracks, right-click a track header or mixer strip, then choose
**Groups… → Create group from selected tracks…**. Groups initially link volume, mute and solo. Open a
group's menu to add/remove selected tracks, change its color or links, or
suspend it. Volume changes preserve members' dB offsets; pan links preserve
pan offsets. Hold Alt (Option on macOS) when starting a fader or pan gesture
to adjust just that member.

Enable **Edit** to select and edit matching items across the member tracks.
Items must have the same start/end at project sample resolution and the same
lane. Moves, trims, fades, mute, splits, duplicates and deletes stay together;
notes and take contents remain independent. A locked matching item refuses
the whole linked edit.

Choose **Add VCA** in the group menu for a mixer fader that controls the
members without moving their own faders. Its mute and solo affect the group;
its **env** button opens normal volume/mute automation lanes. Suspending the
group stops linked editing while its VCA remains audible. Combined member and
VCA levels stop at Console 9's native fader top (about +12.0412 dB). Agents can
inspect requested and applied levels with `track.describe` or `track.list`,
and nest controls with `vca.members.set`.

### Freezing a channel

Choose **Freeze** from a track header or mixer strip's context menu to record its
items, take effects and complete insert chain to project media. It records in
the background; the window shows no progress and cannot stop it, and if it
fails the channel simply stays unfrozen. When it
finishes the channel shows a small snowflake, its items are dimmed and marked, and its
plugins are released. Fader, pan, mute, solo, their automation, sends and parent
routing remain available. To change the sound inside the recording, choose
**Unfreeze** from the same menu. Each completed operation is one undo step.

Agents use `track.freeze {"track":"tr_1","job":true}` (following it with
`job.get`, stopping it with `job.cancel`) and `track.unfreeze {"track":"tr_1"}`. `track.list` reports frozen state and cache
metadata; `session.describe` marks frozen channels. Work on other tracks can
continue during a freeze. Completion refuses if the source sound, input files,
project/branch or latency schedule changed; submit the freeze again.

Disarm the channel and remove external audio, sidechain/key or MIDI inputs first. Arming needs an
unfrozen channel (arming a frozen one says to unfreeze it first), and an armed channel cannot be
frozen, so a recording does not normally meet a frozen channel; arming a group leaves its frozen
members unarmed, with a warning. Should a channel be frozen under a recording anyway, nothing is
lost: the recording stops as usual, every other channel's takes are placed, and the frozen channel's
recording is kept in the project's recordings folder, named in a `record.frozen_skipped` warning.
Unfreeze the channel and `project.recover` places it where it was played, loop passes included. A
commit that is refused for any reason keeps the recordings the same way, and its error says so.
V1 requires unity master playrate without an active rate envelope. Tempo, meter,
project sample rate and master-rate edits name the frozen tracks that must be
unfrozen first. Missing plugins or unavailable media must be repaired before
unfreezing. Infinite or unsettled tails refuse capture; finite reported tails
and a one-second quiet window are retained, up to ten minutes of tail. A bridge
that discovers a longer tail after rendering asks for in-process isolation.

Freeze keeps a particular offline performance. A random or transport-sensitive
plugin may sound different on a fresh pass, seek or loop; internal live DSP
memory cannot be restored by a saved patch. Keep plugin binaries and plugin-owned sample files stable
during capture. Audio after an unreported delay longer than the quiet window
cannot be predicted. Playback at a different device rate resamples the cache.

Frozen files are kept like recorded media, including after unfreeze, because
undo, branches, autosaves and saved projects can still refer to them. Save and
Save As carry them with the project. Do not manually edit or remove those files
while any saved project or history still needs them.

## Routing

The routing window's **Audio | MIDI | Hardware MIDI | Hardware Audio** buttons select the connections to edit.
MIDI mode shows channels and rack Note Outs down the left, channels and
Notes modules across the top. Double-click an empty cell to connect them
(double-clicking a send only selects it), or select cells and use Add/Remove
(or Delete). Set the tap, Mute,
From channel (All or 1–16), and To channel (Original or 1–16) below the grid.
Post-fader follows the source's mute; pre-FX and post-FX keep sending while
the source is muted. Right-click Mute to learn a controller. A channel
can forward MIDI through several other channels without an instrument.
The selected mode is saved with the window layout.

A Notes module's Plays track menu offers None to hear only explicit MIDI
sends and its chosen keyboard. Home restores its rack's channel. **Hear MIDI
sends**, checked by default, also plays the MIDI that other channels and
Note Outs send into the track it plays; a project from before MIDI sends
opens with it unchecked wherever a Note Out sends into that track, so it
sounds as it did. A Note
Out's **MIDI channel** knob chooses channel 1–16 for outgoing notes; choose
its destination in the MIDI routing matrix.

Hardware MIDI routing is the routing window's third tab, **Hardware MIDI**.
Inputs and channels/Note Outs are rows; channels and device outputs are columns.
Double-click to connect, select and Remove to disconnect. Connecting an input
selects the channel's MIDI input, replacing hardware audio or a rack Out;
a channel has one input at a time.
Route an input through a channel to reach a device output. Taps, mute and MIDI
channel controls work as in the MIDI tab. Device connections save by name and
warn if missing on reopen.

The fourth tab, **Hardware Audio**, connects device inputs to channel record
inputs and masters to device outputs. Double-click an empty cell to assign it;
select connected cells and Remove (or Delete) to disconnect. Choosing an audio
input replaces the channel's MIDI or rack input, as the status line explains.
Channels and busses reach hardware through a master. Device channel names appear
when available; saved channels outside the current device say unavailable.
When the engine is stopped, stored channels say "engine stopped".
The actual output used by LTC is labelled `[LTC]`; if the configured channel
is absent, LTC uses the device's last output. A `!` cell warns
`LTC replaces <master>'s audio on output N`.

Right-click a stereo input, output, channel or master header and choose
**Split into L/R** to see and assign its sides, or **Join** to put it back.
**Split all into L/R / Join all** changes the whole tab. The view saves with the
project's window layout and does not change any sound by itself. A single
input channel chosen for a joined channel is mono; a single output chosen for
a joined master plays the mono sum. An outlined **M** marks a mono assignment;
hover names the input/output channel and its mono behaviour. Double-click its
joined pair cell to assign the complete stereo pair. An input with more than
two channels shows its whole map on hover; double-clicking a lit cell keeps
that map, including the extra recording channels. To change just L or R,
split both the signal and the device pair and choose the new channel. A master
needs an existing stereo pair; a record input can start mono or stereo. The
other side stays assigned. Changing one side of a mono record input keeps the
other side on its original mono input. Isolating or removing just one side
without the other is not supported by these assignments; disabled cells and
the status line explain why. To remove a split assignment, select all its
connected cells, or join its signals first. Gain, pan and tap remain on the
channel or master; this tab has no separate send controls.

Right-click a hardware input row or output column header for **Enabled** and
**Timing offset…**; outputs also have the clock items described in "MIDI
clock and MTC to other devices". Positive milliseconds delay; negative milliseconds advance.
The range is -1000 to 1000 ms. A live input can advance only within its current
audio callback. Outputs also offer **Reset notes on stop** (on by default),
which releases notes and sustain on stop or locate. Engine shutdown always
releases notes. These settings are saved for this machine, outside projects.
Output alignment can add up to one second of audio latency to make an advance
possible; this also delays live input monitoring on every channel. The timing
offset dialog previews the shared added latency; per-port `lead_ms` and engine
status report it. While lead is needed, transport changes keep its history
running, and changing the lead uses a 5 ms crossfade between delay taps.
Zero-lead projects keep no running history; adding the first negative-offset
route primes its delay with silence once. Separate device MIDI channels avoid shared
sustain/controller resets between routes.

**Add plugin…** puts a note effect (an arpeggiator, a chord or MIDI effect)
at the front of a channel's FX chain and an instrument just after the note
effects there, so the instrument plays what they send.

A plug-in that sends notes changes what comes after it in its channel's FX
chain. The instrument after it plays its notes, and so do the channel's
post-FX and post-fader MIDI sends, including sends to hardware outputs;
a pre-FX send still carries the channel's own MIDI. Its **MIDI out** button in the FX window chooses
**Replace** (the default: only its notes go on) or **Merge** (its notes and
the ones it heard). Bypass it to let the original notes through. Plug-ins
without audio, only notes, sit in the chain like any other. A frozen
channel's sends play its own MIDI again, since its plug-ins no longer run;
freezing warns about it.

Every plug-in in the chain that takes notes hears them, a second instrument
or a MIDI-triggered gate after the first one included. To keep the older
behaviour, where only the first such plug-in hears the channel's MIDI and
nothing a plug-in sends goes anywhere, click **MIDI: through the chain** beside
the chain's heading (shown when a plug-in there sends notes, or two or more
take them) and
choose **Only the instrument hears MIDI**. Projects saved before this version
open that way on every channel with two or more plug-ins, so they sound as
they did; switch it back to hear the new routing.

**View > Routing matrix**, or a track's **out** button, opens a grid of what
feeds what: a row per track (and per output pair of a multi-out instrument),
a column per destination. A square with a dot is a send; a crossed-out square
is not allowed, and hovering it says why.

Click a square to select it. Ctrl+click adds or removes squares, Shift+click
selects the rectangle from the last square you clicked, and clicking a row's
or column's name selects all of it (Ctrl and Shift work there too). Click the
empty corner above the row names, or press Ctrl+A, to select every square.

Double-click a square to put a send there. **Add** puts a send on every
selected square that can have one, and **Remove** (or Delete) removes the
sends on every selected square. On a track's parent square these turn the
parent send on and off.

Every control under the grid acts on all the sends you have selected: the
tap (Post, Pre, Pre-FX), the level fader, the pan control, the channels menu,
polarity and mute. Dragging the fader or pan moves every selected send by the
same amount, in one undo step; a double click resets them. The controls show
the send on the square you clicked last. Send moves are recorded into the
send's envelopes like the track fader's (see Automation).

A mixer strip lists its sends under its plugins, two at a time; scroll the
list (or drag its bar) to see more. Clicking a send opens the routing matrix,
like **out**. Right-click a send and choose **Delete**, or Alt-click
(Option-click on macOS) it, to remove it.

### Outputs and several masters

Steppe opens every output of the device (up to 64; with JACK, one per
playback port). The main master plays to outputs 1 and 2. A project can have
more masters (right-click the mixer's empty space, **Master**), for example
a headphone mix or a second pair of speakers: each has its own fader, effects
and outputs. Click a master's **out** button in the mixer to choose where it
plays: *None*, a stereo pair (1/2, 3/4, …) or a single output in mono. A new
master plays nowhere until you choose. Masters that share outputs add up, and
all masters reach the device at the same moment, whatever their effects'
latency. The metronome plays on outputs 1 and 2. The menu lists the outputs
only while the audio engine is running.

Agents can print rack notes with `rack.capture {async:true, ...}` and follow
`job.get`; `job.cancel` leaves the project unchanged. Completion adds one
undo step. Rack or automation edits and overlapping destination item edits
need a new capture; destination fader moves and distant item edits are allowed.
Notes source MIDI and incoming routes are snapshots from launch. The rack window
captures the same way.

Rack plug-in latency is compensated against the song, including oversampling.
The rack's longest routed audio or Note Out path sets its clock; shorter
routes receive padding. Parallel paths summed inside the rack keep their
relative timing; add a Delay module when the patch needs alignment.

Sends into rack In ports apply the route's level and pan without Console 9
trim. Existing effect patches fed by those sends play about 2.34 dB louder
than earlier builds; check their output level when updating.

A rack's Out modules can also send straight to a buss or master, beside the
channel they feed: in the routing window's Audio mode each Out has a row
under its channel ("Pad » Out 1", teal), and only buss and master cells take
a send. A rack send has a level and a mute, nothing else; pan and polarity
belong in the rack. It joins the buss after the buss's Console 9 decode,
unencoded, so it does not get a channel's summing character (route the Out
into a channel and send from there for that). Muting or soloing the rack's
channel mutes or solos its rack sends too; its fader does not move them. So
muting the channel to hear the drum rack's reverb alone silences the reverb
as well: pull the channel's fader down instead. The routing window's status
line names the channel each Out row follows; moving the rack to another
channel (or removing its channel, when another channel also hears it) makes
the sends follow that channel. Removing a rack's only channel removes the
rack and its sends; for agents, `track.remove` names them in a warning.
Freezing the channel leaves its rack sends playing live, so the rack still
uses its CPU, and locks the rack until you unfreeze: you can still move its
modules, rename them and change the sends, but not its knobs, cables,
plug-ins or MIDI learn bindings. Agents use
`route.add {from: {module: "mo_3"}, to: "tr_5"}`.

### Printing a mix onto its master

A master has an arm button in the mixer, like a channel. Arm it and press
record: Steppe records what the master plays, after its effects, fader and
pan, as the music goes. The take lands on the master itself, in a lane that
appears at the top of its tracks (a master armed or holding a take has one;
the lane goes away when it has neither). Several masters can record at once,
each its own take. Punch and loop recording work as on a channel.

A master's takes are already mixed, so they play after its effects and fader,
which do not change them; only the master's own mute silences them with the
rest. They play beside the live mix, so right after a print you hear the mix
twice. The **M** in the lane's header is the lane mute: it silences the
master's takes and nothing else, to compare the print with the live mix or to
keep it silent. While a master records, its earlier takes are silent in the
punch range (everywhere when punch is off), so an old print never ends up in
the new one; outside the punch range they play as usual. Recording while the
song already plays can let an old print be heard for a fraction of a second
before it goes quiet; it is never recorded.

Takes on a master are audio items: move, trim, split, fade and copy them as
on any channel, or drop an audio file onto the lane. A MIDI item cannot go on
a master. A render of the master includes its takes; stems of channels and
busses leave them out. Projects that use master takes or an armed master do
not open in versions of Steppe from before this feature.

Arming a channel or turning its monitoring on or off while the song plays
does not interrupt it: an instrument's notes from the timeline and an
effect's tail keep sounding, at any playrate. The exception is the first
change since audio started that sets a playrate other than 1× while the song
plays: if that same change arms a channel or turns its monitoring on, a note
the channel holds stops. A plug-in run in a separate process is not heard on
a monitored channel at all. Notes you hold on the keyboard end when the
keyboard stops being heard (disarmed or monitoring off) and whenever the
monitor mode changes, for example from On to Auto.

When monitoring an armed input, the latency comes from the device, input FX,
and effects on that input's route to the master. Effects on unrelated playback
tracks do not delay it. If the input feeds both a direct path and a bus with
latency, the shorter path is delayed so the two stay aligned.

## Automation

A track's **env** button lists its volume, pan and mute envelopes, then the
same three for each of its sends ("Send → Reverb · Volume"). A send's lanes
appear under the track it leaves, after the track's own. Record a send's
moves as you would the fader's: set the lane's mode to touch, latch or write
and move the send's level or pan in the routing matrix while playing.

While you write a level or pan (a track's or send's level or pan, or a VCA's
level), you hear the control as you move it rather than the envelope's older
points. In latch and write mode it keeps the last value until you stop or
pause, which is also what the envelope stores. In touch mode the envelope
takes over again half a second after your last move; holding the control
still for longer than that hands back to the old automation. Mute plays
from its envelope while you write it.

A plug-in's knob works the same way: while you hold it in the plug-in's
own window, the envelope stops moving it, and what you set is what is
recorded. In touch mode the envelope takes over when you let go; in latch
and write mode the last value holds until you stop or pause. A modulation
that reaches that parameter through its envelope pauses while you hold it
and resumes on top of what you recorded.

An item's own effects (its take FX) are automated on the item's track, in
song time, and their lanes run the whole width of the timeline. They play
only under the item, and only while that take is the one heard, so the lane
is dimmed everywhere else; you can still edit points there.

Undo, redo or switching branches while playing starts a fresh automation
pass at the current position. Earlier moves no longer override the envelope
or extend their last value to where you stop.

To edit an automation item's points, its amplitude magnitude must be greater than
0.000000001. If it is zero or too small to invert, the app asks you to restore
the amplitude first; the source curve is preserved.

If another edit changes an automation item's amplitude, baseline or geometry
while you drag its points, header or edge, the drag is cancelled with a message.
Start the drag again using the updated item.

## Modulation

A **modulation** moves a parameter or a mixer level around the value it has,
following a signal from a rack: an LFO, a random source, a macro, or the level
of another track. It adds to the value, so it works on top of the fader or
automation instead of replacing them. In the app you set one up from a
control's right-click menu (**Modulate**, below); agents use `modulate` and
`modulation.*`.

Agents can cable modulation to a plug-in parameter by semantic key or its
CLAP id (`fx_3.gain` or `fx_3.12`; `effect.12` in a named `rack.patch`).
Numeric ids use canonical decimal spelling: `12`, not `012`.
`fx.describe` lists the ids, including third-party plug-ins without semantic
keys. The target must support modulation. Numeric semantic keys take precedence.

A Mod Out module takes a rack's signals out of the rack: `modulation.add
{source: {module, input}, target: {fx, param}, depth}` routes one of its
inputs to a parameter of a plug-in on a track, where `fx.describe {full:
true}` marks the parameter `modulatable`. Depth is the offset at a full
signal, a fraction of the parameter's range (−1 to 1); routes on one
parameter add up, and none of them writes the parameter's value or its
automation.

A VST3 parameter, or a CLAP one the plug-in does not mark, can still be
modulated when it can be automated and is continuous: `fx.describe` marks
it `modulatable_by_envelope`. Its envelope gives the base, and the
modulation is added to what the envelope sends, so it needs an active
envelope. `modulate` adds one, with a single point at the current value,
and `modulation.add` asks for one (`env.add`). Turning the envelope off
leaves the route idle (`inert: "envelope"`). Recording automation on such
a parameter writes the base: the modulation's current offset is taken off
what the plug-in reports. Switches and parameters with a few fixed values
cannot be modulated. `modulation.set` changes a route (a drag that sends one
gesture is one undo step), `modulation.remove` removes it, and
`modulation.list` shows each route's signal range, the modules cabled
into its Mod Out input and, when it cannot play, why: `source` when
nothing is cabled into that input yet. Removing the Mod Out or the plug-in removes its routes with a
`modulation.removed` warning.

A route can also take a track's volume (`{track, param: "volume"}`, depth
in dB, −48 to 48), pan or width (percent, −200 to 200), or the level of an
audio send to a buss or master (`{route, param: "volume"}`). The offset
adds to what the fader or its automation gives and the sum stays in the
fader's range; between the 32-sample steps it moves in a straight line,
so a fast modulator sounds smooth.

`modulate {target, source}` does all of that in one undo step. `source`
is `lfo`, `random` (an LFO set to Random Steps), `macro` (a Macro's first
knob) or `follower` with `from`, the track whose level it follows. The
command uses the first rack on the target's track that has a Mod Out,
else the first rack named Modulators. If neither exists, it adds a rack
on a new channel named Modulators. It adds the module, cables it into
the first free Mod Out input and makes the route. A follower also gets
an In module and a send from `from`. When that rack would feed back
into itself, the command uses another Modulators rack, or adds one. That
happens when the rack already drives the track the follower listens to,
or already listens to the target, and each rack it adds that way comes
on another new channel named Modulators. A follower cannot listen to the track
it modulates, or to a track that track feeds. Without a depth the
command uses a quarter of a parameter's range, 6 dB, or 50 %. The source
starts at its module's defaults: set its knobs with `module.set {module,
params}` on the module the command returns (an LFO's `rate`, `shape` and
`sync`, a follower's `gain` and `threshold`; a Macro's `macro_1` sits at 0
until moved, so a macro changes nothing at first), and the depth with
`modulation.set`. For a macro, the result's `knob` is the knob that moves
the target.
`modulation.telemetry` reads each route's live offset and how far it
can move its target, and `engine.status` counts modulation steps that
had to wait for room in a block (`modulation_deferred`).

In the app, right-click a fader or pan, or a parameter's row in the FX
window's parameter tab, and choose **Modulate**: it lists LFO, Random,
Macro and a follower for each other track, and runs `modulate` with your
choice. It is greyed where the parameter cannot take modulation. A
modulated control shows how far its modulation can move it as a violet bar
beside it, and while the song plays, where it is now; drag that bar to
change how deep the modulation goes. A plug-in without its own parameter
descriptions lists its parameters in the FX window; those it lets you
modulate have a slider there.

A violet "~" says that something is modulated. It appears at the right of
a plug-in in the mixer when one of its parameters is, and after "fx" on
the track (in the track list and the mixer) when any plug-in in its chain
is. Selecting the track shows a **Modulation** row in the inspector: each
modulated parameter with its source ("Marram · Filter 1 Cutoff ~ LFO"), or
"5 routes" when there are more than four. Click a line to open the rack
window with that source picked. A modulated parameter's lane has a
**mod** button that does the same (with a short list when several sources
move it), and its title names the parameter first ("Filter 1 Cutoff ·
Marram"). On a fader, pan or FX parameter, the right-click menu's **Show
modulation source** takes you there too; clicking the violet bar only
changes the depth.

## Timing audio with hits and stretch markers

Items have two menus. Right-click an item for the everyday edits: cut, copy,
paste, duplicate, delete, consolidate, heal, split, mix to new track, the MIDI
editor and rendering through FX. The menu bar's **Item** menu has the rest for
the selected items: export, what the items follow when the tempo changes, audio
timing (hits and stretch markers), take FX and envelopes, and the takes.

Right-click selected items and choose **Consolidate items** to combine material
per track, including MIDI items on several tracks at once.
Audio and MIDI remain separate. Audio uses active takes and bakes their item
controls, timing, take FX and envelopes into a new WAV; track processing stays
live. Gaps stay silent. MIDI items on a track become one MIDI item, with gaps
left empty and overlapping notes layered; a single MIDI item with several takes
has its takes layered into one. Progress appears in the status line; all
results publish together in one undo step.
Select complete linked/warp bundles on matching lanes and groups. Undo retains
the original takes; the new media stays available to history and project saves.
Agents use `item.consolidate {items:[...]}` and follow the returned job with `job.get`.

**Heal splits**, beside Consolidate items, joins compatible audio pieces without
making a new file. Agents use `item.heal {items:[...]}`.
Select consecutive unchanged pieces from the same source on each track, including
any linked/warp peers. Healing keeps all corresponding takes, the outer fades and
the full source/warp mapping, removing generated internal slice fades. Source
settings and stored take FX must match; moved pieces, changed fades and nonlinear
envelope seams that cannot be restored need consolidation instead. Each track
becomes one item and the operation is one undo step.
Both menu entries are greyed when the selection cannot use them.

For items with several takes, **Item > Keep only the selected take** deletes
every take but the one you hear (click a take's lane to choose it) on each
selected item, in one undo step. **Item > Delete take** removes just the take
you hear.

Right-click selected items to render them through their FX, audio and MIDI
alike (MIDI plays through its track's instrument), in one undo step. **Render
with FX, replacing the audio** puts the result in place of the take you hear;
**Render with FX as a new take** keeps that take and adds the result beside it.
**Render with FX, deleting FX** replaces the take as well and then deletes the
effects the render went through: the take's and those on the items' tracks, so
they do not process the result again. Other items on those tracks lose the
track effects too. On the item you right-click, the take under the pointer is
the one rendered.

Dragging an item's edge trims that edge of every selected item by the same
amount, on any track and audio or MIDI alike; Alt on the right edge stretches
the audio items and trims the MIDI ones. Locked items stay as they are.

Right-click an item and choose **Split item at cursor**, or press S, to split
selected audio and MIDI items at the edit cursor. Items outside the cursor stay
whole; the menu's pointer position does not set the split point. Undo restores
the entire selection edit in one step.

**Item > Mix to new track** mixes the selected items into one new audio file,
as a buss would: each item plays through its track's effects, automation, fader
and pan, and the result goes onto a new track, created muted so it does not
double what is already playing. Unmute it (and mute or remove the originals)
when you want to hear it instead; this saves CPU and joins many files into one.
Only the selected items are heard, whatever the tracks' mute and solo say;
sends and the tracks' parent busses are left out. The new file runs from the
first selected item's start to the last one's end (effect tails beyond that are
cut) and is always a stereo WAV at the project's rate; the render dialog's
settings do not apply. Progress shows in the status line, and one undo removes
the new track. Agents use `item.mix-to-track {items:[...]}` and follow the job.

Select an item and choose **Item > Export…** to save its material to a file of
your choosing. An audio item gives a WAV of its source audio exactly as recorded
or imported: no gain, fades, effects or mixer processing, at the source's own
sample rate and channels (a 16- or 24-bit WAV source stays 16 or 24-bit; others
become 32-bit float). Only the part the item covers is exported: a trimmed item
gives its trimmed part, and one piece of a split item gives only that piece. A
looping item repeats its source as you hear it. Play rate and stretch markers
choose which stretch of the source is exported but are not applied. A MIDI item
gives a MIDI file of the notes it plays, starting at zero, with the tempo and time
signature at the item's start. The entry is greyed for an empty take; the status
line says where the file went. Agents use `item.export {item, path}`.

**Split item at prior zero crossing** moves the split back to the last point,
at most 100 ms before the cursor, where the item's audio crosses zero, which
avoids a click at the cut. If there is none, it splits at the cursor. It does
nothing to MIDI items. Linked items split together at the same point.

Select audio items and choose **Item > Detect hits** to show candidate
attacks. Drag a dim hit to create a stretch marker, or drag a solid marker handle
to change timing while retaining pitch. Hold Shift for a move off the grid. Item
edges stay pinned when the first marker is added. **Remove warping** returns to the
take's original scalar rate/pitch and leaves detected hits visible.

To put an anchor at a specific time, set the edit cursor there, select the
items and choose **Item > Add stretch marker at cursor**. It inserts a marker in each
selected audio item containing the cursor, without moving its timing. Drag the
marker to change timing later. Existing markers are not duplicated; MIDI items
are skipped. The command is one undo step and keeps warp peers together. As with
other warp edits, it needs nonlooped audio in Signalsmith pitch-preserving mode.
Agents can use `take.stretch.add {take, item_pos}` with `item_pos` in seconds
from the item's start; omitting `source_pos` preserves the current timing.

**Quantize audio** moves attacks to the current arrange grid. **Slice at hits**
splits without moving them and adds short fades. Both detect hits automatically
when needed; undo the timing edit first, then detection if desired. Agents can
also quantize slice positions, choose sensitivity/strength/swing, and merge hits
from several selected items. Several aligned drum-microphone items use one set of
hit times; warp bundles process their channels together to retain relative phase.
Select the entire bundle for further timing edits or use `audio.warp.detach` to
separate it deliberately. Gain, pan and FX remain independently editable.

Use nonlooped audio with pitch preservation and the Signalsmith algorithm. Linked
items must share start/end positions, rate and pitch. Remove warping before slicing.
Quantization that would cross hits or land on an item edge refuses with a hint to
choose a finer grid or lower strength. Slicing can leave faded gaps; it does not
invent sustained tails. The previous audio continues while “Preparing audio…” is
shown. Source preparation finishes before the new timing becomes audible.

## Racks

A rack is a modular patching space that plays into a channel. **View >
Racks**, the toolbar's **Racks** button, or **Add rack** in a channel's
right-click menu opens the rack window: a tab per rack along the top (**+**
adds one on the selected channel) and the modules on a plain space below.
Each new rack starts with an **Out** module, which becomes its channel's
input. While that is all it holds, the space says **Add a module, or start
from a patch**: click either to open the browser on its modules or its
patches. Every rack's Out is also listed in each channel's input menu, so
any channel can take any rack's sound.

Middle-drag or hold Space and drag to pan; the wheel zooms at the pointer
(10 to 400 %), Ctrl+wheel (Cmd+wheel on macOS) scrolls vertically, and
Shift+wheel or a sideways wheel scrolls across. Home shows everything, Shift+Home zooms to the selected
modules and Ctrl+0 is 100 %. Zoomed far out, each module shows just its
name, so a big patch stays readable. Each rack remembers where you left it.

The browser on the left (the **Browser** button shows or hides it) has
two views. **Modules** lists what you can add: search it, or click a tag
to filter. Double-click a module or drag it into the space. **Plugin…**
puts any CLAP or VST3 plugin in the rack as a module; click it to open
its editor. **Patches** lists whole patches: your own under *Yours*, and
the ones that come with Steppe under *Factory*. Hover one to read what it
is, search them, and double-click one or drag it into the space. Into an
empty rack a patch arrives whole, as its author laid it out and in view,
using the rack's own Out; Undo takes it away in one step.

The quickest way to add a module is to start typing its name over the
space, or double-click an empty spot: a list opens where the pointer is,
and Enter puts the module there. Drag a cable from a jack and let go on
empty space, and the module you pick from the same list arrives plugged
into it.

Drag a module by its body to move it, Ctrl+click, Shift+click or drag a
box on the space to select several, nudge them with the arrow keys
(Shift: finely), and Delete, Backspace or Alt+C removes them. Ctrl+C and
Ctrl+V copy and paste the selected modules with the cables between them (paste lands at the
pointer, in this rack or another, with plug-ins' settings as they were
when you copied), Ctrl+X cuts and Ctrl+D duplicates.
To keep what a sequencer or any module with pitch and gate outputs plays,
right-click it, choose **Capture notes to track…**, then a channel and a
length: the notes land there as a MIDI item, from the bar the playhead is
in, and Undo takes them away. While it renders, the status bar shows how far
it has got, with **Stop**; if you change the rack or that channel meanwhile,
nothing is added and the status bar asks you to capture again.
A rack's performance controls are its **Macro** modules: cable a Macro's
outputs to what you want to play, and the channel the rack feeds shows those
knobs at the top of its FX chain window (the **fx** button), named after
what they drive ("Filter cutoff"), with **Open** to go to the rack. Turn
them there, right-click one to automate it or to learn a MIDI controller,
and watch them move with their automation as the song plays.
In the mixer, that channel shows the rack as a line above its plugins,
marked **rack**: click it to open the rack. A rack that holds a Mod Out
module is a modulator for the track it lives on. That track's strip shows
it as a line marked **mod** in violet, the colour of modulation, which
opens the rack the same way. In the FX chain window the
rack shows those knobs; double-click it anywhere (or click **Open**) to
open the rack.
**Tidy up** in the space's right-click menu lays the rack out by signal
flow, left to right; with several modules selected, only they move. Undo
puts them back.
Hover a module, or select some, and their cables stand out while the rest
fade back.

Drag from a jack to another to connect them. Jack colours show what a jack
carries: audio, pitch, gates, control voltages and phase each have their
own colour, and a jack and its cables glow with its level. Drop a cable on a knob to
modulate it; the ring around the knob shows how far, and dragging the ring
changes the amount. Drag a cable off an input to move it, or drop it on
nothing to remove it. A cable that loops back is dashed: it carries the
signal one sample late. Right-click a cable to see what it reaches, type
its amount, or remove it.

Some modules' displays answer the mouse as well: drag or scroll on them to
change what they show (each drag or scroll is one undo step), and move the
module by its title instead.

Drag a knob up or down to turn it (Shift for fine), double-click to reset
it, and right-click it for **Show lane**, which adds an automation lane for
it under the rack's channel, or **MIDI learn…**: move a control on your
MIDI controller and click OK, and that control turns the knob (a small
dot marks it), which moves on screen as you turn it; right-click it again
to see the binding or remove it. Right-click a module to rename, bypass,
duplicate, randomize or delete it, to load one of its presets, or to **Show
source**. A **Notes** module's menu also chooses what it plays: **MIDI
input** (None plays its channel's keyboard while the channel is armed and
monitoring; Any or a named device plays that keyboard always, armed or not)
and **Plays track** (Home, the rack's channel, or another channel's MIDI
items and keyboard). Modules from your library (in the data folder's `modules`) can be
edited and saved there and then; a stock module offers **Make editable
copy**.

To keep a patch, right-click the space and choose **Save rack as
patch…**, or select some modules and choose **Save as patch…** from
a module's menu. It is listed under **Patches**, *Yours*: double-click it,
or drag it into the space, to add its modules to the rack you are looking
at, and right-click it to delete it. **Load patch…** in the space's
right-click menu opens the Patches view. Right-click the space and choose **Add
module…** to pick a module from the column and put it where you clicked.

Patches that play from the song (a Transport module, sequencers, clocks:
Rain Bells, Song Machine and the other generative ones) stay silent until
you press play; while the transport is stopped, the rack window's status
bar says so. Instrument patches need notes: MIDI items on their channel.
Pressing play, or moving the playhead while playing, starts every module
afresh, as plug-ins are reset there: an LFO starts its cycle again from
its offset, a delay or reverb drops its tail, a filter its ringing, a
cable patched back into the rack (feedback) starts from silence, and a
knob you just turned is at its new value at once instead of still
gliding. So the rack sounds the same from a given point on every play and
in every render, whatever it played before. A loop going round does not
restart them: an LFO runs on and tails carry across the loop point. While
the transport is stopped a rack runs on as it was.

Some stock modules play a little differently than in earlier versions,
and a project saved then plays them the new way: a project does not keep
its own copy of the stock modules, and there is no setting for the old
sound. They used to do some of their work once per block the audio
engine handed them, so how they sounded after you moved the playhead
could differ between playing and exporting; they now keep their own
steady 32-sample pace. You may hear it in these cases:

- modules on a loop of cables (one with a dashed cable): **Harmonic**,
  **Resonator**, **Delay** (its Wow and Flutter), **VCO** with Drift, and
  **Drum** or **Sequencer** with cables on their knobs follow their
  cables and wander every 32 samples instead of every sample;
- **Slew**, and a **Macro** whose knob is moving, glide from the very
  start of playback (or a move of the playhead) instead of following
  their input unglided for the first 32 samples;
- **Markov** clocked from a cable: just after a reset, a note's length
  can change by up to 31 samples;
- **Reverb** with **Freeze**: when Freeze ends, the damping comes back
  at once instead of up to 31 samples later, so the tail is slightly
  darker there.

A rack sends audio both ways. Its Out modules are channel inputs, and an
**In** module takes sends, and the rack becomes an effect. Send a track to
it in the routing matrix (**View > Routing matrix**), where each In module
is a column named after its rack ("Shimmer » In 1"), next to the rack's
channel. A double click on a cell sends; the level, pan, tap and channel
controls work as for any send. A rack's own channel, and any track that
channel feeds, can't send into it, since that would feed back. A **Notes** module plays its channel's
MIDI (or another track's, or a MIDI input's) as pitch, gate and velocity
cables, a voice per note; **Transport** gives the song's clock. A plugin
module's own parameters take cables too: drop a cable on the module and
pick a parameter from the list (type to search). It modulates the
parameter around where its knob stands, half the range each way at first.
The cable ends in a small socket on the module's top edge: hover it to see
the parameter, and drag it up or down to change the amount. Only CLAP
plug-ins take modulation, third-party ones too, when their parameters accept
it; a VST3 plug-in's list says so.

A track has one input: audio, MIDI or rack. Choosing one replaces the previous
input. The input menu starts with **No input**, which clears any selection;
audio inputs, MIDI devices and rack Outs follow in separate sections. When a
selection-wide audio or MIDI choice replaces rack Outs, the status line names
the affected channels.

To play a rack from a keyboard, select the keyboard on another channel,
arm it and turn monitoring On (Auto listens while stopped or recording).
In the routing window's MIDI tab, send that channel to the rack's Notes
module. Keep the rack's Out selected on its audio channel. Arm the MIDI
channel to record notes; arm the rack channel to record the sound as WAV;
arm both to record both in the same pass. A new channel starts with the
monitor mode set in Preferences. Arming or changing monitoring while the
song plays keeps timeline notes sounding; held keyboard notes end when
that keyboard stops being heard.
Deleting a rack's channel deletes the rack too, unless another channel
still hears it (its input is the rack's Out, or a Note Out plays into it):
the rack then moves to that channel and keeps its name.
`module.set {device: null}` removes an explicit Notes device (`""` listens
to every MIDI input, like `"*"`), and `source: null` returns Notes to its
rack's home channel.

Older files with multiple inputs keep the rack input first; otherwise MIDI
when the track has a main FX chain, otherwise audio. A load warning names
each affected track and the dropped input, with recovery advice. Files from
2.8 or earlier whose keyboard selection had disconnected an unambiguous rack
Out reconnect that Out and drop the MIDI input, with the same warning. Other
MIDI-only selections stay MIDI. When multiple rack Outs are unrouted, a
warning asks you to choose the intended Out in the channel's input menu.
Reconnections that would create feedback also retain MIDI with a warning.

Your own modules live in the library folder, and the project you save
keeps a copy of each one it uses beside it (in `modules`), as it keeps its
media, so the project sounds the same on another computer. Rack presets
(saved in the data folder's `rack-presets`) hold a group of modules with
their cables and settings, and the source of your own modules too.
If a project copy cannot be loaded, opening the project reports the source
problem and whether a library version is used instead or a type is unavailable.

Agents can do all of this through the MCP tools: build a patch in one
step (`exec {"command": "rack.patch"}`), listen to it as numbers
(`exec {"command": "rack.probe"}` reports pitch, levels, gate rates and the
notes a sequencer plays), write and edit modules
(`exec {"command": "module.write"}`, `exec {"command": "module.edit"}`; the
modules playing one are rebuilt at once), save and load rack presets,
automate knobs and bind
them to MIDI controllers (`exec {"command": "module.learn.set"}`).

## MIDI controllers

Right-click a track's fader, pan, mute, solo, polarity, mono, arm or
monitoring button, in the track list or the mixer, and choose **MIDI learn**,
the first entry of its menu. The pan menu offers **Pan** in pan mode, **Pan left** and **Pan right** in
dual mode, **Pan** and **Width** in width mode, and **Gain compensation** in
every mode on channels and busses. The positions follow knobs with soft
takeover; compensation learns as a toggle.
Move the knob, fader or button on your controller, then click OK. Each track
can have its own knobs, so a controller with eight faders can drive eight
tracks. A VCA's fader, mute and solo in the mixer learn the same way. The
same works in the routing matrix: right-click the level, pan, mute, polarity
or a tap button to bind it on every selected send.

These bindings are saved with the project and undo like any other edit. The
menu shows what a control is bound to ("CC 7 · ch 1"); open the dialog again
and click **Remove** to free it.

- A fader or pan follows a knob (CC mode Absolute), or an endless encoder in
  one of the three Relative modes. With **Soft takeover** on, the knob only
  takes over once it reaches the current value, so it never jumps.
- A button flips on each press: a pad (a note) always does, and a CC button
  does in Toggle mode, the dialog's default for buttons. In Absolute mode a CC
  button holds it on while pressed (127) and off when released (0).
- Monitoring steps through off, on and auto; a send's tap button selects
  that tap.
- Moving a bound control records automation like moving the control itself.

Plug-in parameters learn from the **MIDI** button in the FX window, and any
action from the actions list (`?`, then Ctrl+L). Some actions follow a knob
instead of running once per press: the selected tracks' volume and pan, the
master volume, the playrate, the last touched plug-in parameter, and
**Routing: last touched send (MIDI CC)**, which moves the level or pan of the
send you moved last in the routing matrix. For the volume, pan and playrate
actions the dialog offers **Soft takeover** too, as it does for a fader (the
last touched parameter and send always take over softly). Action bindings
are your own settings, shared by every project.

### Hardware MIDI output device status

`engine.start` opens available MIDI destinations alongside inputs. Inspect
`engine.inputs.midi_outputs` for names, and `engine.status` for
`midi_output_warnings`, `midi_out_dropped` and `midi_output_latency` (signed
frames, audio output latency minus MIDI output latency). A busy device is
skipped while audio keeps running; close the application using it and restart
the engine to retry. Device topology changes are advisory and require an
explicit restart to adopt the new set. `midi.inputs.get`/`midi.outputs.get` show
per-port settings and warnings. Opening outputs alone sends no music; add
hardware routes in the routing window.

## Sync

A project has a timecode of its own, Steppe can follow another machine,
and it can send MIDI clock and MTC to other devices.

### Project timecode

**Project > Timecode…** sets the project's timecode: its frame rate (23.976,
24, 25, 29.97 drop-frame, 29.97 non-drop or 30 fps) and its start, the
timecode at the project's first beat (01:00:00:00 is common for picture
work). New and older projects use 30 fps from 00:00:00:00. Type the start
as HH:MM:SS:FF (with `;` before the frames in drop-frame) and press Enter.
The setting is saved with the project and undoable, and timecode sync and
LTC output use it.

### Following another machine (sync)

Steppe can follow a master: a drum machine or another DAW sending **MIDI
clock**, a video deck or DAW sending **MTC**, or a tape machine or video
system sending **SMPTE LTC** as audio. Right-click the tempo button in the
transport bar and choose the source; **Sync settings…** in the same menu
(or Transport > Sync settings…) chooses where it arrives:

- **MIDI clock** and **MTC** arrive on a MIDI input: choose it in **MIDI
  input**.
- **LTC** arrives on an audio input: choose the channel in **LTC input**.

When the master plays, Steppe starts at the same place and follows it
by changing its playrate slightly, with the pitch kept, so it stays with
the master even if their clocks differ. When the master stops, so does
Steppe; when it jumps, Steppe jumps too. With MIDI clock, **Follow the
master's tempo** plays the project at the master's tempo (a 126 bpm master
plays a 120 bpm project 5% faster); without it, only Start, Stop and Song
Position are followed. MTC and LTC positions use the project's timecode
(Project > Timecode…): a master at 01:00:10:00 plays a project that starts
at 01:00:00:00 from its tenth second. **Offset** moves Steppe later
(positive) or earlier against the master, and **When the signal stops**
either stops at once or keeps playing for a few frames, for signals with
gaps.

The tempo button then reads **EXT** and the tempo heard: green when locked
to the master, orange while it is catching up, red when nothing arrives.
The project's own playrate is not changed, and renders are unaffected.
You can record while following: takes are placed where the master was.
While following, Steppe processes the whole project live, as when
monitoring an input, so very heavy projects may need a larger buffer.
When it follows the master's speed (MTC, LTC, or MIDI clock with Follow
the master's tempo), audio items play through the time stretcher, which
sounds very close to, but is not exactly, normal playback.

**LTC output** (off until you turn it on) sends the transport's timecode as
LTC on an output channel you choose (it replaces anything a master sends to
that channel), so a tape machine, a camera or another DAW can follow
Steppe. The timecode is the position of the audio leaving with it, so
plug-in latency does not put it early. If the chosen channel is not on the
current device, LTC goes to the device's last output instead, and the
dialog says so; your choice is kept for when the device has it. It is
silent while the transport is stopped.

Real devices' timing has not been measured on every system yet.

### MIDI clock and MTC to other devices

A drum machine, sequencer or video system can follow Steppe over a MIDI
output. In the routing window's **Hardware MIDI** tab, right-click the
output's column header: **Send clock**, **Send transport**, **Song Position
Pointer**, **MTC** and **Clock while stopped** (once Send clock is on) turn
each on or off, beside **Reset notes on stop** and **Timing offset…**. An
output that is sending clock shows a small clock before its name. An agent
sets the same with `midi.outputs.set`, one output at a time, for example
`{"port": "Interface", "clock": true, "transport": true, "spp": true}`:
`clock` sends MIDI clock while playing, `transport` Start, Continue and
Stop, `spp` the song position (so a device starts mid-song in the right
place), `mtc` MIDI time code at the project's timecode, and
`clock_while_stopped` keeps the clock running at the tempo while stopped.
`offset_ms` moves that output's MIDI later (or earlier, negative) to suit
the device, notes and clock alike, and `enabled: false` silences the output.
An offset earlier than the audio's own latency delays all audio by the
difference (`lead_ms` shows it), so the clock still lands on time.
The port is the output's name as `midi.outputs.get` lists it (any letter
case, as long as only one output matches). Clock and MTC are off until you
turn them on, and the settings are kept with your preferences, also for a
device that is not connected now.
The messages are timed with the audio you hear, so plug-in latency does
not put them early; nothing is sent during a count-in, and an output on
the device Steppe follows (Transport sync) does not get its own clock
back. `midi.outputs.get` and `engine.status` show what each output sends
and how many messages went out.

## The clip launcher

Besides its timeline, a channel can hold **clips**: short pieces of MIDI or
audio, each in a **scene**. A scene is a column of clips you can play
together, a section of a song before it has a place in the arrangement.
`scene.add` makes a scene, and `clip.add {track, scene, notation |
audio_file | midi_file | item}` makes a clip, from notation, a file or a
copy of an item. A MIDI clip's length is in beats and follows the tempo;
an audio clip's is in seconds and plays at its take's own rate. A clip
loops by default (`mode: once` plays it once), and a loop window can
repeat part of it. `item.add {clip}` lays a copy of a clip on the
timeline.

### The launcher pane

**View > Clip launcher** shows the pane between the track panel and the
timeline; it opens by itself in a project that has scenes. Each scene is a
column, each channel a row, so a clip sits beside its track's controls.
Drag the pane's right edge to widen it; dragged all the way, it takes the
whole view and the timeline hides until you drag it back. Its width, and
whether it is shown, are kept with the project's view settings.

Above the columns, **Stop all** silences every clip and **Arrangement**
returns every track to the timeline. Each scene's header has a ▶ that
launches the scene; double-click its name to rename it, drag it sideways
to reorder the scenes, and right-click it for Rename, Colour, Insert at
playhead, Insert at end, the two checks for how scenes are laid, Lay out
scenes… and Delete (all described below). The **+** after the last scene
adds one; until you name it, it shows as "Scene" and its place, such as
"Scene 4".

Stop all, Arrangement and Capture sit in one row. In a narrow pane they
show as small symbols (a square, two bars and a dot); rest the pointer on
one and the status line names it.

**Capture**, after Arrangement, records what you launch the way the record
button records audio. Click it so it lights, then play and launch clips and
scenes. When you stop (or seek, or change the loop) the performance is
written onto the timeline as items, in one undo step, and Capture stays lit
for the next take; click it again to finish and turn it off. If every track
stayed on the arrangement, nothing is written. Once the take is written with
the transport stopped, every track goes back to the arrangement, so pressing
play plays what you just recorded rather than the clips again. Capture also
lights when an agent is capturing; clicking it then finishes that capture
and writes it.

If the performance can't be written (a locked item is in the way, say),
Capture goes dark and the status line says why. The performance is kept: fix
what it names, then click Capture, and it is written before Capture lights
again. If it is refused again, the status line says so, and one more click tries
once more and, if it still can't, discards the performance and starts a new
capture.

To put a scene into the song, drag its header onto the timeline: it lies
from the bar you drop it on, for as many whole bars as its longest clip
needs, with a marker showing where while you drag. **Insert at playhead**
and **Insert at end** in its menu do the same from the next bar line at the
playhead, or after the last item or section. Each makes a section named
after the scene. Two checks in the same menu say how the clips are laid,
for a drop too: **Follow the song's key and chords** lays the MIDI clips so
they sound in the key and chords of the song where they land (unchecked,
each clip keeps its own Follow, set in its inspector; following only the
key, or forcing a clip to play as written, is set there per clip), and
**Write harmony to the context track** writes the clips' own key and
chords onto the context track under the new section, unless the track
already has points there or the clips disagree about the key or chord at
some point (the status line says which). The project remembers both, and
recalling a screenset leaves them as they are. **Lay out scenes…**
lays several scenes one after the other: tick the scenes you want, set each
one's bars and the bar to start from, and choose **Lay out**. Each
scene's bars start at what its clips need where it would land, and follow
the scenes before it as you tick and change them, until you type a scene's
bars yourself. If it can't
be done the dialog says why, marks the scene it is about with a red dot,
and keeps what you entered. The other way round, right-click a section in
the ruler and choose **Capture as scene** to make a scene from the items
inside it.
When one of these cannot be done (items already lie in the way, a scene has
no clips, a section has two items on one track) the status line says why. It
names the clips, items, tracks, scenes and sections involved; one without
a name is called by where it is, such as "Scene 4", "Bass's clip in Verse"
or "Keys's item at bar 5". A clip without a name shows no name in its cell,
since its row and column already say its track and scene.

In a cell, no click does two things:
- a clip's ▶ launches it; a click on the clip's body selects it (Ctrl adds
  more) and a double-click opens it: a MIDI clip in the piano roll, an
  audio clip in the same pane as its waveform with its envelope lanes;
- on a track with clips, an empty cell's small square stops the track; a
  click elsewhere in the cell selects it, and a double-click makes an empty
  one-bar MIDI clip there and opens it.

A clip shows its notes or waveform under its name. While it plays, its ▶,
its outline and a line along its foot that shows its progress turn green;
a clip waiting for its quantize boundary has a dashed outline. A scene
whose clips all play has its header outlined. The narrow **Arr** column at
the right has a dot per track, lit while the track follows the
arrangement; click an unlit dot to return the track. While a clip holds a
track, the track's items on the timeline are dimmed.

Drag a clip to another empty cell to move it, with Ctrl to copy it; an
occupied cell refuses it. Drag a clip onto a channel's lane to lay a copy
on the timeline, or an item into an empty cell to make a clip from it.
With several items selected, each one makes a clip on its own row, kept
at the same distance from the item you hold, as when you move them.
Several items on one track go into that scene and the ones after it, in
the order they play: the first into the scene you drop on, the next into
the one after it. One undo takes them all back. Every cell they will
fill is outlined while you hold them. If any of them can't go (its cell is
taken, its row is not a channel, it would fall off the track list, there
are not enough scenes after the one you drop on, or it is on a hidden
track or in a closed folder), none is made and the status line says why.
Right-click a clip for Rename, Colour, Mode (loop or once), Quantize,
Duplicate, Lay on timeline at playhead, Take FX… (the clip's own effects,
which play whenever the clip does) and Delete; Delete on the keyboard
removes the selected clips. Ctrl+C copies the selected clips; click an empty
cell and press Ctrl+V to paste them there, laid out as they were. Ctrl+X
marks clips to move, and the next paste moves them. Copied clips stay in
their own project: after you open another one, there is nothing to paste. The inspector shows a selected clip's name,
colour, length, gain, fades (an audio clip's), mode, launch quantize, Follow and mute. Follow (key or
chords) maps the clip's notes onto the song's key or chords, when it is laid
and when it is launched; it needs a key at the clip's start. When you lay a clip or a
scene on the timeline, its notes are laid as written and the status line at
the bottom says how they sound: "Bass follows: sounds in D minor, +2" for a
clip that follows the song's key, or "Keys is laid as written: its key C
minor differs from D minor at bar 21" for one that doesn't, under a
different key ("its chord Am differs from F at bar 5" in the same key over
another chord). A Capture take laid under a different key or chord says so
the same way. A clip with its own key
shows it in the corner of its cell ("Dm", "G dor"), with "→" before it
when the clip follows. The pane draws a clip dimmed when launching it
now would be refused: an audio clip with take FX or take envelopes while
the master playrate keeps the pitch (or follows MIDI clock), a clip
whose audio didn't fit in memory, a frozen track's clips, or a clip
just added while audio is being prepared (it stays dimmed until the
audio is ready). A scene's
▶ is dimmed when the scene can't launch: nothing in it can play, or a
clip it would start with the rest is refused. A refusal that depends on
the transport, such as the engine not running, is not shown. A click
still asks, and the status line says why.

In the piano roll a clip starts at its own bar 1, and its end is marked
"end", with the time after it shaded. It has no place in the
song, so it keeps the key and chords it was written against, and the roll
shows those: the key button names its key, its chords are in the strip over
the roll (keys in a row under the chords), and the rows follow its key and
the note names its key and chords. Drag a chord or key along the strip to move it (with Alt, a
chord is copied). Right-click the strip
to set its key (**Key…**), add a chord, remove one, take the song's key and
chords at the playhead, or clear them; double-click a chord to change it.
Without a key the key button is off. While the clip plays, the playhead
shows where.

An item laid from a clip that follows keeps the clip's key and chords, and
its notes stay as written: they sound in the song's key (or chords) under
the item, so they move when you change the song's chords later. The item
shows "→D" (the key it was written in) in its name bar. To make a part you
wrote on the timeline follow the song from now on, select its item and set
**Follow** to Key or Chords in the inspector (or **Follow the song's key**
in the Item menu): it takes the song's key and chords under it as its own,
so nothing changes until the song does. **Bake** (or **Bake into notes**)
writes the notes as they sound and stops following. A key or chord change
on the ruler says in the status line which items followed it ("Bass and
Keys follow the new key at bar 9: G major"). In the piano roll such an item
is drawn where its notes sound, with the written pitches outlined;
**Written** shows the notes as written instead. The strip shows the song's
chords and, under them, the item's own ("take"), which you edit as a
clip's. With the key button lit, dragging a note steps it by the scale's
degrees, as it sounds. (Playback doesn't follow yet: for now an item plays
its notes as written, though the roll and the item's thumbnail show where
they will sound.)

Notes outside the key at their place are outlined in amber, and the roll's
title counts them ("3 outside A minor"). That is how you see a clip laid into a song in another
key without following: its notes play as written, and the ones that clash stand out.

An audio clip opens in the same pane, as its waveform: time in seconds
from the clip's start, its end marked, and under it the same **Lanes**
menu and envelope lanes as a MIDI clip's (the take's volume, pan and mute,
and its effects' parameters), placed in seconds. Points snap to a grid of
seconds that gets finer as you zoom in; **Snap** turns it off. The key
button there is
the clip's key: click it to set one, remove it, or take the song's at the
playhead. The pane doesn't edit the audio itself.

### Playing clips

A track either follows the arrangement or plays one of its clips.
`launch.clip {clip}` starts a clip on its track. `launch.scene {scene}`
starts every clip of a scene together, on the same beat. Tracks with no
clip in that scene keep playing what they play. `launch.stop {track?}`
silences a track's clip, and `launch.arrangement {track?}` returns a
track to the timeline. Without `track`, they act on every track a clip
holds.

A launch takes effect at the next quantize boundary: the clip's own
`quantize`, else the project's `launch_quantize` (a bar by default).
With `none` it happens as soon as the engine can still take it: about one
audio block ahead for a track the audio callback runs, about 0.2 s for
the others. Launching a clip while stopped starts playback with it from
the play position. A track whose clips have take FX with latency waits
that much longer, so the clip is still heard on its boundary. At the
switch, notes still sounding end and audio crossfades over 5 ms. Returning to the arrangement does not restart a
note already under way: it sounds from its next start. An audio clip can
fade in and out on every pass (`clip.set {fade_in, fade_out}`, in seconds,
with the same shapes as an item's fades; in the inspector, **Fade in** and
**Fade out**, each a length and a shape, and the clip's cell shades the faded
part as an item's fades are shaded). Its own fades replace the 5 ms
switch: a launched clip with a fade-in starts with its own fade, and a
clip launched away during its fade-out finishes that fade-out, overlapping
the incoming clip by up to the fade's length. A clip keeps its
place against the bars, so relaunching a loop of whole bars stays in
phase.

A clip's take FX and its take envelopes play as they do on the timeline
(`fx.add {take}`, `env.add {take}`), and an instrument in its take FX plays
its notes. The take's envelopes run from the clip's start, in beats on a
MIDI clip and seconds on an audio one, again on every pass, and so do the
envelopes on its take FX (`env.add {fx}`). Effects start afresh at each
launch, and their tail is cut when the track switches away.

`launch.state` tells what each track with clips plays as you hear it:
`arrangement`, `clip` (with its position in the clip), `held` (a `once`
clip that has finished, silent until the next launch) or `stopped`. It
also shows what each track has queued, and for a clip that plays or is
queued, `followed`: what it follows by (`none`, `key` or `chords`).

### Capturing a performance

`launch.capture {on: true}` starts keeping what you launch; `launch.capture
{on: false}` stops, at once, or with `quantize` (`"bar"`, `"beat"`, …) on
that grid's next line, so the performance ends on the bar. The Capture
button, turned off while playing, ends it on the project's launch quantize
and commits it there. `launch.commit` then writes the performance onto the
timeline as ordinary items, in one undo step. It is laid out as it ran:
from where the capture began, each launch as many seconds later as it was
heard, even if the transport loop jumped back meanwhile. On each track that
played a clip or was stopped, those stretches replace what the timeline had
there; tracks that stayed on the arrangement are left alone. Each clip
becomes copies of its takes, starting where the clip was in its loop, with
the 5 ms switch crossfades as item fades, and an audio clip's own fades on
the items where its passes start and end. A clip still fading out when the
capture ends is laid to the end of its pass, as you heard it; on that
track the items under that tail are removed and the next one starts with a
5 ms fade-in, and the commit's reply ends there. When an
item's fades cannot say what you heard (the capture began inside a clip's
fade-in, the loop jumped inside a fade, or a switch cut short a fade-out),
the commit is refused with `launch.commit.fade` and the performance kept:
set the clip's fades to 0, or capture it again. MIDI is copied, so editing the
items leaves the clips alone; `launch.commit {pooled: true}` shares it
instead. Stopping, seeking or changing the loop ends a capture, and
committing again writes the same performance over itself.

Renders play the arrangement only; the render dialog says so while a
clip is playing. Not yet:
- launching an audio clip with take FX or take envelopes while the
  master playrate keeps the pitch (`launch.playrate`); other clips
  follow the playrate, audio ones time-stretched with the pitch kept, and
  a launch then lands at least about 61 ms ahead, while its stretcher
  gets ready (anything launched on that track meanwhile lands after it),
  and a launch pressed while a playrate change takes effect, or queued
  before it and not yet begun, can land a boundary later;
- capturing while a launched audio clip plays and the master playrate
  keeps the pitch (`launch.playrate`): stop the clip or turn off preserve
  pitch first. Launching an audio clip then ends a capture at its switch,
  with a warning. Otherwise a capture runs under any playrate and through
  changes to it, and the commit plays the same song positions, not the
  same durations, at another rate.

A rack's Notes module that plays a track's notes follows that track's
launches: it plays the clip, falls silent when the track stops, and
plays the timeline again with the arrangement. The track's MIDI sends
carry the clips too.

A frozen track plays its print: `launch.clip` refuses its clips
(`track.frozen`), and the pane draws them dimmed, and `launch.scene`
skips them.

### From scenes to a song

A scene is a section of a song before it has a place in the arrangement.
`scene.insert {scene, bars}` lays a scene's clips on the timeline, one item
per track, starting at a bar (`bar`, or `at` in beats; the default for
`arrangement.build` is bar 1) and lasting `bars` whole bars, and marks the
span as a **section** named after the scene. A `once` clip lasts its length
(or the span, if shorter); a looping clip lasts the span, as one looping item.
`arrangement.build {sequence: [{scene, bars}, …]}` does that for a list of
scenes in turn, a scene as often as you like. `scene.capture {region}` goes
the other way: it makes a scene, named after the section, from the items that
lie inside it, one clip per track. Each is one undo step, and none of them
changes the audio or the sections that were already there.

They copy the MIDI, so editing a clip afterwards does not change the items it
made (`pooled: true` shares it instead), and they refuse rather than guess:

- a scene with no clips (`scene.empty`);
- items already lying in the span on a track that would get a clip
  (`scene.insert.occupied`: nothing is overwritten, trimmed or split);
- a clip that repeats at something other than its content, such as a gap after
  its notes (`scene.insert.period`; not laid out yet);
- for `scene.capture`, a track with several items in the section, an item that
  crosses the section's edge or starts after its start, or a section with no
  item (`scene.capture.items`, `.edge`, `.offset`, `.empty`). Items on a master
  are skipped with a warning. An audio item's fades go to its clip and back;
  a looping audio clip with fades goes on the timeline as one item per pass.

When you write notation into a clip with `midi.write`, the clip keeps its
length: notation longer than the clip is kept but does not play, and the
command warns (`midi.beyond_length`). Lengthen the clip first with `clip.set
{length}`. `session.describe` lists every scene with its clips, their length,
mode, loop, quantize and gain.

## Rendering

**File > Render…** opens the render dialog; **File > Mix** renders the mix
straight away with the project's render settings; its progress and the file it
wrote show in the status bar.

### One set of settings per project

Each project keeps one set of render settings. They are saved in the project,
belong to its branch and can be undone like any other edit. The dialog opens on
them and writes them back only when you press **Render** or **Add to queue** and
something changed; that change is one step in the history, *Render settings*.
Closing the dialog without rendering keeps the project as it was. File > Mix and
agents' renders use the same settings, so what you set up in the dialog is what
they render.

What a project leaves unset comes from **your default**: **Save as default**
stores the dialog's settings for every project, and values that come from it
are drawn in the quieter ink. **Presets** are named settings of your own, plus
read-only factory presets: WAV 24-bit, a streaming master (−14 LUFS, −1 dBTP),
a CD master (16-bit 44.1 kHz with noise-shaped dither, −1 dBTP), a podcast MP3
(−16 LUFS), stems for mixing (24-bit, mono tracks as mono) and a game loop
(Ogg Vorbis, seamless). Choosing a preset loads it into the dialog; Render
writes it to the project. Presets and your default are kept in the data folder
(`render-presets`, see *Where Steppe keeps its files*).

### Sources and ranges

- **Master mix** renders the main master. With more than one master, a list
  under it chooses which masters to render, each to its own file named after
  the master (`-{track}` is added to the file name unless it already has
  `{track}`). **Stems** renders each checked top-level
  track to its own file, isolated (no master processing) or through the
  master, after the fader, before it or before the effects; *Skip silent
  stems* leaves out tracks that stay below −90 dBFS. **Regions** renders each
  region, with *Handles* adding seconds before and after it.
- **Version** renders the current mix, another branch's head or an earlier
  version from the history, for this render only. Every result names the
  branch and version it came from, and its menu offers *Branch from this
  version* to start a branch at that mix.
- **Range**: the entire project (to the last item's end), the time
  selection (the range drawn on the ruler, also the loop, whether or not
  looping is on), or a custom start and end in beats. A **tail** renders seconds past the end for
  reverb and delays; *Cut silence at the end* trims what is left quiet.

### Files and formats

All renders go to **one folder for every project**, shown in the dialog with
**Choose folder…** and **Open**. The **file name** is a pattern: **Tokens…**
inserts `{project}`, `{version}`, `{branch}`, `{track}`, `{region}`,
`{date:YYYY-MM-DD}` and the others, and a `/` makes subfolders, so
`{project}/{project}-v{version}` keeps each project's renders in a folder of
its own. **If the file exists** chooses Overwrite, Add a number (`-2`, `-3`…),
Skip, or Ask, which lists the existing files before rendering.

Each render can write up to four files of the same audio (**+ Output**): WAV
(16 or 24-bit, or 32-bit float), AIFF, FLAC, MP3 (constant or variable bit
rate, with a gapless header) or Ogg Vorbis, each with its own bit depth,
dither (TPDF or noise-shaped) and sample rate. Later outputs whose extension
repeats get a suffix (`mix.16.wav`) unless you give one.

### Loudness and delivery checks

**Loudness** normalizes to an integrated (LUFS-I), short-term or momentary
loudness, a sample or true peak, or RMS, with targets for the usual platforms
(−14 LUFS for Spotify and YouTube, −16 for Apple Music and podcasts, −23 for
EBU R128, −24 for ATSC A/85). *Only turn down* never raises the level; for
stems, *Same gain for every stem* applies the master mix's gain to all of
them, so they still add up to the mix. **Limit true peak** works with or
without normalizing. When a target or a ceiling is set, every file is measured
as written (MP3 and Vorbis decoded first) and the results show a pass or a
FAIL with the measured value; *Write a loudness report* puts a text report
beside each file.

**Processing** trims silence at the start or end, and *Seamless loop* folds
the tail back into the start so a game loop repeats without a gap.
**Metadata** writes title, artist, album, track, date, genre, ISRC, comment
and a cover image from the project folder where the format holds them (the
dialog says where it cannot), BWF timecode and loudness in WAV files, and the
project's markers and regions as WAV cue points or AIFF markers.

**After render**, *Add to project* puts the files on new tracks, and *Open
folder when done* (a preference) shows them in the file manager. **Add to
queue** pins a render to the project as it is now; **Queue** runs them all.

## Agents (MCP)

Steppe is built to be driven by an AI agent as well as by hand. `steppe-mcp`
serves the running app's commands as MCP tools over stdio. What an agent
does lands in the same undo history as your own edits. The bundled skill
teaches the agent Steppe's commands, units and notation.

By default, the engine exposes 38 common command tools plus `describe`, `exec`
and `query`; the running GUI also exposes `gui_selection` for its selection. `describe` with no arguments lists the command groups, one line each;
pass `group` for a group's commands and `command` for one command's full schemas,
errors and examples, or `text` to search names and summaries in any case. Call
other commands with `exec {"command": "fx.patch.list", "args": {"fx": "fx_2"}}`,
or `query` for read-only commands. Dedicated tool definitions leave out result
schemas; `describe` provides them when needed. For clients that defer tool
loading, add `--tools all` to `steppe-mcp` or `steppe-engine --mcp` to expose
every non-Internal command as a dedicated tool. `--tools core` explicitly
selects the default. The plugin sandbox keeps its existing dedicated commands.

The proxy loads the command index once and fetches descriptions only for the
selected dedicated tools. Use `describe` for other contracts when needed; `exec`
validates through the running application. Use the proxy and application from
the same installation: startup requires exposure metadata in the command index.
After an app restart, the proxy checks the catalogue on the next tool request
and notifies the agent when it changes. If an edit loses its response, inspect
project state before deciding whether to retry it.

Saved `dsp.test` audio and `dsp.preview` pictures use readable, shortened
filenames with a hash when a plugin ID needs escaping or truncation, or ends
in the reserved `-` plus 16 hexadecimal digits suffix. Stock plugin filenames
stay the same. Use the returned `path` to open them; the `plugin` field retains
the full plugin ID.

### Connecting from the app

**Help > Connect an agent…** registers this copy's MCP server with Claude
Code, Codex or Hermes:

1. Choose the agent and review the command shown.
2. Click **Connect**, or **Copy command** to run it yourself. Copied commands
   are for PowerShell on Windows.
3. Restart or reconnect the agent. Start Steppe before you use the agent.

The result shows the command's exit status and output. If a server of that
name is already registered, check its path in the agent before replacing it.

Connect also links the bundled skill into the agent's user skills folder, as
`steppe/`:

| Agent | Skills folder |
| --- | --- |
| Claude Code | `~/.claude/skills` (or `$CLAUDE_CONFIG_DIR/skills`) |
| Codex | `~/.agents/skills` |
| Hermes | `~/.hermes/skills` (or `$HERMES_HOME/skills`; `%LOCALAPPDATA%\hermes\skills` on Windows) |

The dialog shows the path before you click. If a skill of that name is
already there, or in Codex's older `~/.codex/skills` (or `$CODEX_HOME/skills`),
Connect leaves it as it is and says so.

**Help > MCP server…** shows this copy's absolute paths, a headless command
and stdio/JSON instructions for any MCP client. **Help > Download skill…**
copies the bundled skill into a folder you choose, under `steppe/`.
**Download skill link…** creates a symlink (a directory junction on Windows)
instead, which follows updates to the installation; keep the installation in
place when you use one. Use these for other agents, or to replace a skill
that Connect left in place. Replacing an existing skill or link asks first,
and unrelated files and folders are never touched.

On macOS, move Steppe.app into Applications before connecting an agent. If
macOS runs it from a temporary App Translocation location, Help shows how
to restart it from a stable location instead of these controls. See
[Installing Steppe](installing.md#macos-apple-silicon-macos-144) for
clearing quarantine.

### Connecting by hand

The installed MCP adapter and skill are here:

| Platform | `steppe-mcp` | Skill |
| --- | --- | --- |
| Linux | `~/.local/bin/steppe-mcp` (installed) or `bin/steppe-mcp` in the extracted tree | `share/steppe/skill` in the tree |
| macOS | `/Applications/Steppe.app/Contents/MacOS/steppe-mcp` | `Steppe.app/Contents/Resources/skill` |
| Windows | `%LOCALAPPDATA%\Programs\Steppe\steppe-mcp.exe` | `skill` in the installation folder |
| Build tree | `build/<preset>/mcp/steppe-mcp` | `skill/` in the repository |

Register the adapter by its absolute path, and link the skill into the
agent's skills folder. For a build tree, from the repository root in a
POSIX shell (Linux or macOS):

```sh
claude mcp add -s user steppe -- "$PWD/build/release/mcp/steppe-mcp"
mkdir -p ~/.claude/skills
ln -s "$PWD/skill" ~/.claude/skills/steppe

codex mcp add steppe -- "$PWD/build/release/mcp/steppe-mcp"
hermes mcp add steppe --command "$PWD/build/release/mcp/steppe-mcp"
```

Start Steppe first, then the agent (in Claude Code, `/mcp` reconnects). If
Steppe restarts while the agent runs, the next tool call reconnects by itself.
To drive a second running copy, start it with `steppe --instance NAME` and
pass `--instance NAME` to `steppe-mcp`.

Replace `release` with your build preset (`macos` on macOS). On Windows,
use the app's **Copy command** button for a PowerShell command with this
installation's paths.

Without the GUI, `steppe-engine --mcp` serves the engine's project and audio
commands and can render offline. GUI-only state and actions require the app.
`steppe-engine play FILE --seconds 10` plays a saved project for ten seconds;
add `--backend dummy` to run without an audio device. Numeric options must be
complete numbers, and `--seconds` must be finite and nonnegative. Invalid values
report the option and how to correct it before playback starts.

For headless recording, stop the transport and save the project before closing
an MCP connection or stopping the socket server. Shutdown finishes capture but
does not save the project. Unsaved audio and MIDI recordings can be recovered
(File > Recover unsaved work…, or `project.recover`); other session edits are
lost when the process exits.

MIDI sends, including a rack's Note Out routes, can be recorded by their destination channel. Arm
it in MIDI replace or MIDI overdub mode, then record; loop and punch apply
as for device MIDI. The channel needs no instrument or device input, and a
selected MIDI device can record alongside the rack's notes. An explicit MIDI
record mode records these sends even if audio or a rack Out is selected.

In input record mode, an armed channel whose input is a rack Out records stereo audio through its
input FX, before its own items, inserts and fader. It uses ordinary audio
takes, loop/punch recording and recovery. Disable the live rack feed when
auditioning the recorded item to avoid doubling it.

Recorded filenames use sanitized track names and take numbers. Tracks named
`Vocal` and `vocal` receive distinct take names when recorded together, including
on case-insensitive filesystems.

Audio takes begin as ordinary WAV files and become RF64 automatically above the
32-bit size limit, without interrupting recording. Steppe can import and recover
both formats, including recordings made with the older WAV header layout.

Audio takes keep an append-only placement journal beside each WAV. After a
crash, Project recovery restores loop passes and seeks through the last complete
checkpoint; extra audio bytes are kept without guessing their placement. Stop
keeps a final placement sidecar, and the next successful save/autosave retires
the journal. The loop range at recording start is used for recovery; changing
it during the take has the same limitation as MIDI recording. Punch changes
continue to apply at stop; crash recovery restores the punch from the last
complete recording checkpoint, independently of the current transport punch.
During recording the punch must contain at least one sample and fit the
recording clock; a refused change leaves the range unchanged.

Output recording is not supported yet. If a loaded project has an armed track
in output record mode, recording fails before creating any files. Switch that
track to input mode with `track.set {track: "tr_1", record_mode: "input"}`.

### Other clients and local models

Any MCP client can use the same server. Give agents without skill support
`skill/SKILL.md` as instructions; it lists the guides to read when needed.
The tool list is large (over 140 tools). If you run a local model behind a
harness that sends every tool with each request, give the model a context of
32k tokens or more. llama.cpp's `llama-server` needs `--jinja` to accept
tools.

### Agent transport limits

Control/MCP are local same-user endpoints. RPC queues at most 1,024 pending
requests and 64 MiB globally; excess calls receive `rpc.busy` and should be
retried later. Each peer permits 64 subscriptions. Request IDs are at most 1,024
serialized bytes, and untrusted frames at most 1,000,000 JSON values.

MCP stdio uses a 64 MiB writer reservation: keep reading replies even after
closing stdin. Output backpressure pauses new commands while model service
continues. EOF drains replies before finishing capture/shutdown; an open consumer
that stops reading can defer that completion. Oversized replies return an explicit
error. If a reply is lost, inspect state before repeating an edit: it may have run.

If control-socket startup reports `rpc.path` for the product runtime directory,
choose a writable private directory owned by your user and remove any symlink
or Windows directory junction at that product directory. Steppe refuses these
aliases rather than changing their targets' permissions.
