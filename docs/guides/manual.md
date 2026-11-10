# Steppe manual

This manual covers using Steppe once it is installed ([Installing
Steppe](installing.md)) or built ([Building Steppe](building.md)): starting
the app, audio devices, projects, plugins, and connecting an AI agent.

The workflow follows REAPER's, so most of the arrange view, mixer, FX chains,
routing and actions list will feel familiar from it. The actions list includes
the public commands and GUI actions (`Shift+/` or `Ctrl+Shift+P`,
`Cmd+Shift+P` on macOS).

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

If the device is missing or busy when Steppe starts, the app still opens. The
Audio device dialog then opens and says why. Choose another device, or
**Restart audio** once the device is ready. The dialog's status lines show
what is running: the backend, rate, buffer size, device period and output
latency.

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

Agents can cable modulation to a plug-in parameter by semantic key or its
CLAP id (`fx_3.gain` or `fx_3.12`; `effect.12` in a named `rack.patch`).
Numeric ids use canonical decimal spelling: `12`, not `012`.
`fx.describe` lists the ids, including third-party plug-ins without semantic
keys. The target must support modulation. Numeric semantic keys take precedence.

Rack plug-in latency is compensated against the song, including oversampling.
The rack's longest routed audio or Note Out path sets its clock; shorter
routes receive padding. Parallel paths summed inside the rack keep their
relative timing; add a Delay module when the patch needs alignment.

Sends into rack In ports apply the route's level and pan without Console 9
trim. Existing effect patches fed by those sends play about 2.34 dB louder
than earlier builds; check their output level when updating.

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

## Projects

A project is a JSON file. Its media lives in the `media/` folder beside it.
Project and FX chain files accept oversampling factors 1, 2, 4 or 8. If a
hand-edited file uses another factor, loading reports the field and allowed values.

Until a project is saved, its recordings go to a folder of its own under the
data folder (below), in `unsaved/<start time>-<pid>/media/rec`. The first
save moves them into `media/` beside the project file. Quitting, or starting
another project, without saving deletes that folder after the app has asked.
A crashed run leaves its folder behind, recordings included.

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
still for longer than that hands back to the old automation. Mute and
plug-in parameters play from their envelopes while you write them.

Undo, redo or switching branches while playing starts a fresh automation
pass at the current position. Earlier moves no longer override the envelope
or extend their last value to where you stop.

To edit an automation item's points, its amplitude magnitude must be greater than
0.000000001. If it is zero or too small to invert, the app asks you to restore
the amplitude first; the source curve is preserved.

If another edit changes an automation item's amplitude, baseline or geometry
while you drag its points, header or edge, the drag is cancelled with a message.
Start the drag again using the updated item.

## Panning

Right-click a channel's or buss's pan, in the track list or the mixer, to
choose how it pans:

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
the others (0, −2.5, −3, −4.5 and −6 dB) give the strip its own. 0 dB, the
default, never cuts or boosts, so a centred signal keeps its level through
any number of busses. A cut law applies at every strip a centred signal
passes. **Gain compensation (boost pans)** keeps the centre at 0 dB and
boosts the sides instead, which can clip a loud recording. A master's pan is
a plain balance, with no mode or law.

## Routing

**View > Routing matrix**, or a strip's **out** button, opens a grid of what
feeds what: a row per strip (and per output pair of a multi-out instrument),
a column per destination. A square with a dot is a send; a crossed-out square
is not allowed, and hovering it says why.

Click a square to select it. Ctrl+click adds or removes squares, Shift+click
selects the rectangle from the last square you clicked, and clicking a row's
or column's name selects all of it (Ctrl and Shift work there too). Click the
empty corner above the row names, or press Ctrl+A, to select every square.

Double-click a square to put a send there. **Add** puts a send on every
selected square that can have one, and **Remove** (or Delete) removes the
sends on every selected square. On a strip's parent square these turn the
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

Middle-drag or hold Space and drag to pan; Ctrl+wheel zooms at the pointer
(10 to 400 %), Home shows everything, Shift+Home zooms to the selected
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
marked **rack**: click it to open the rack. In the FX chain window the
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
it, and right-click it for **Automate**, which adds an automation lane for
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

Selecting a MIDI input on the rack's channel keeps its audio Out connected:
the channel's input menu checks its audio input (an input, a pair or a
rack's Out) and its MIDI input (**no MIDI input**, any device or one)
separately, and its input button shows both, such as **Bells + Keystation**.
**no audio input** and **no rack input** take off a hardware input or a
rack's Out the same way, so a channel's input can go back to empty.
Arm the channel and turn monitoring On to play it from the keyboard (Auto
listens while stopped or recording). A new rack channel starts with the
monitor mode set in Preferences, like any new channel. Arming or changing
monitoring while the song plays keeps the rack's notes from the timeline
sounding; notes you hold on the keyboard end when the keyboard stops being
heard. On other channels a hardware input and a MIDI input still replace
each other. A rack channel with a keyboard input records the notes you
play; without one it records the rack's sound as a WAV take. To record
both at once, arm a second channel whose input is the rack's Out.
Deleting a rack's channel deletes the rack too, unless another channel
still hears it (its input is the rack's Out, or a Note Out plays into it):
the rack then moves to that channel and keeps its name.
`module.set {device: null}` removes an explicit Notes device (`""` listens
to every MIDI input, like `"*"`), and `source: null` returns Notes to its
rack's home channel.

Projects saved by earlier builds after choosing a keyboard on a rack's
channel had lost the rack's connection to its channel. Opening one
reconnects it when the rack has a single Out, and says so in a warning;
otherwise choose the rack's Out in the channel's input menu again.

Your own modules live in the library folder, and the project you save
keeps a copy of each one it uses beside it (in `modules`), as it keeps its
media, so the project sounds the same on another computer. Rack presets
(saved in the data folder's `rack-presets`) hold a group of modules with
their cables and settings, and the source of your own modules too.
If a project copy cannot be loaded, opening the project reports the source
problem and whether a library version is used instead or a type is unavailable.

Agents can do all of this through the MCP tools: build a patch in one
step (`rack_patch`), listen to it as numbers (`rack_probe` reports pitch,
levels, gate rates and the notes a sequencer plays), write and edit
modules (`module_write`, `module_edit`; the modules playing one are
rebuilt at once), save and load rack presets, automate knobs and bind
them to MIDI controllers (`module_learn_set`).

## MIDI controllers

Right-click a track's fader, pan, mute, solo, polarity, mono, arm or
monitoring button, in the track list or the mixer, and choose **MIDI learn**.
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
mixer strip. The **fx** button opens the
track's FX chain, including when it is empty. In the mixer, right-click a
plugin to rename, duplicate or delete it; a duplicate keeps the plugin's
current settings and goes right after it. Alt-click (Option-click on macOS)
also deletes a plugin.
The chain window's **<** and **>** at the top left step to the previous or
next track, bus or master in mixer order, and clicking its title lists them
all to pick from.
In the chain window, Ctrl-click (Cmd-click on macOS) toggles a row in the
selection, including the active row. Effects moved to another track leave
that window’s selection.

## Agents (MCP)

Steppe is built to be driven by an AI agent as well as by hand. `steppe-mcp`
serves the running app's commands as MCP tools over stdio. What an agent
does lands in the same undo history as your own edits. The bundled skill
teaches the agent Steppe's commands, units and notation.

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
does not save the project. Unsaved audio files can be recovered; unsaved MIDI
recordings and other session edits are lost when the process exits.

A rack's Note Out routes can be recorded by their destination channel. Arm
it in MIDI replace or MIDI overdub mode, then record; loop and punch apply
as for device MIDI. The channel needs no instrument or device input, and a
selected MIDI device can record alongside the rack's notes.

An armed channel whose input is a rack Out records stereo audio through its
input FX, before its own items, inserts and fader. It uses ordinary audio
takes, loop/punch recording and recovery. Disable the live rack feed when
auditioning the recorded item to avoid doubling it.

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

Disarm the channel and remove external audio, sidechain/key or MIDI inputs first.
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
