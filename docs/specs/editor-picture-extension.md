# Spec: CLAP extension `com.suppleaudio.editor-picture/1`

**Status:** Implemented. The extension ID carries the major version;
incompatible changes get `/2`. Additions within `/1` only append members
after a version check (§4).
**Header:** `plugins/common/include/supple/clap/editor_picture.h` (C ABI,
depends only on the CLAP headers), with a byte-identical copy in
`plugins/suite/semantic/include/supple/clap/` so that source exports build
on their own.
**Implementations:** plugin side in the suite layer
(`plugins/suite/semantic/src/semantic_layer.cpp`, over the plugin
template's `Instance::watch()` and `Instance::drawEditor()`, framework
1.24.0); host side in `host/clap-host` (`Instance::watchEditor`,
`Instance::drawEditor`), used by `dsp.preview`.

## 1. Purpose

An agent working through Steppe's MCP server cannot look at a plugin window.
This extension lets a host ask a plugin for a picture of its editor, drawn
offscreen with no window and no display, as it would look if opened now:
its controls at their values, its meters and the plugin's own displays
showing audio the host has just processed, after clicks, drags and turns of
the wheel the host asks for, whose edits reach the plugin's parameters as a
user's would. `dsp.preview` saves it as a PNG, which the agent reads, and
says what the pointer edited, so an agent can check its own interactive
drawing. Hosts that do not know the extension lose nothing.

## 2. Plugin interface

```c
static const char CLAP_EXT_SUPPLE_EDITOR_PICTURE[] = "com.suppleaudio.editor-picture/1";

enum {  /* clap_supple_pointer_input.kind */
    CLAP_SUPPLE_POINTER_MOVE = 0,     /* to (x, y): a hover, or a drag with the button down */
    CLAP_SUPPLE_POINTER_PRESS = 1,    /* the main button, at (x, y) */
    CLAP_SUPPLE_POINTER_RELEASE = 2,
    CLAP_SUPPLE_POINTER_WHEEL = 3,    /* `wheel` notches at (x, y), positive up */
    CLAP_SUPPLE_POINTER_LEAVE = 4,    /* out of the editor */
};
enum {  /* clap_supple_pointer_input.flags */
    CLAP_SUPPLE_POINTER_SHIFT = 1 << 0,
    CLAP_SUPPLE_POINTER_CTRL = 1 << 1,  /* Cmd on a Mac */
    CLAP_SUPPLE_POINTER_ALT = 1 << 2,
    CLAP_SUPPLE_POINTER_DOUBLE_CLICK = 1 << 3,  /* a press: the second of two */
};
typedef struct clap_supple_pointer_input {
    uint32_t kind, flags;
    double x, y;  /* design units: the picture's pixels at scale 1 */
    double wheel;
} clap_supple_pointer_input_t;

typedef struct clap_plugin_supple_editor_picture {
    uint32_t struct_size;
    void (*watch)(const clap_plugin_t* plugin, bool on);
    bool (*draw)(const clap_plugin_t* plugin, double scale,
                 const clap_supple_pointer_input_t* input, uint32_t count,
                 const clap_ostream_t* out, const clap_ostream_t* why);
} clap_plugin_supple_editor_picture_t;
```

All functions are `[main-thread]`.

- **`watch(on)`.** While on, the plugin keeps what its editor shows of the
  audio up to date as if an editor were open (the template's telemetry and
  audio feed; a script's `Shared` members), so that audio processed
  meanwhile shows in a picture. A plugin may cost a little more per block
  while watched. Off by default; the host turns it off when done. Watching
  changes no audio.
- **`draw(scale, input, count, out, why)`.** Makes the editor as it would
  open now and gives it the `count` pointer inputs in order (`input` may be
  null when `count` is 0), as a user's mouse would: presses on controls
  turn them, presses elsewhere go to the plugin's own drawing (a script's
  `pointer()`). What they edit reaches the host as a user's edits do
  (gesture begin, values, gesture end, through the params extension's
  usual path), and every gesture they open is closed before `draw`
  returns. Unknown kinds are skipped. Then it draws the editor at `scale`
  times its design size (clamped to 0.25 .. 4) and writes to `out`:
  - the width and height in pixels, each a little-endian `uint32`;
  - then `width * height` pixels of 8-bit RGBA, rows from the top.

  The picture shows what the input left: a hover lit, a menu open, a drag
  still held if no release came. It returns false, writing nothing to
  `out`, when the plugin cannot draw without a window here (an unavailable
  device/driver or a build without an editor). Input is applied only after
  renderer and font setup succeed; a later readback failure may still leave
  edits applied. `why`, if not null, then receives one line of UTF-8
  saying why. Drawing uses a context and a thread of its own: a GL
  context the host has current stays current. An open editor is not
  disturbed, though the picture's editor may take telemetry frames the
  open one would have shown.

Drawing the same editor state twice on the same platform/rendering setup
produces identical bytes. Pictures need not match across platforms or
machines (GPU or software rasterisers differ in antialiasing); hosts compare
pictures from different setups loosely, if at all.

## 3. Host behaviour

`dsp.preview` (engine and sandbox) makes a fresh in-process instance, sets
its patch, calls `watch(true)`, renders a test signal or notes through it
with the offline renderer, calls `draw` with the pointer actions it was
given (a click is a move, a press and a release; a drag, a press, eight
moves and a release; a double click's second press is flagged), then
`watch(false)`, and writes the pixels as a PNG in the renders folder. It
reports what the input edited as a merge patch from the semantic patch
before `draw` to the one after. Isolated (bridged) instances are not
pictured.

The script player draws headlessly with EGL on Linux/BSD, Metal on macOS,
and WGL on a never-shown window on Windows (framework 1.24.0). Metal uses
an unattached layer and an RGBA texture with synchronized readback; WGL needs
framebuffer objects, which Windows' GDI Generic GL 1.1 driver lacks. No
available renderer, device or driver yields structured `dsp.no_picture` at
`/args/plugin` or `/args/script`, with the renderer's reason and a corrective
platform hint. Native editor windows remain independent of this capability.
CI requires successful pictures and pointer edits on Linux, macOS and Windows;
Windows runners supply Mesa software WGL beside the test executables.

## 4. Versioning

`struct_size` is the size of the struct the plugin was built with; a host
calls no member beyond it, and a plugin whose struct is smaller than this
version's is treated as not offering the extension. New members are only
appended.
