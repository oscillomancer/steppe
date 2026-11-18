#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screensets (gap log #38) end to end: the app saves a named layout into the
project's UI file and recalls it in a later run. Offscreen, no audio device."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run_app(app, project, *actions):
    argv = [app, str(project), "--offscreen", "--no-socket", "--frames", "3", "--audio", "none", "--workdir", str(project.parent)]
    for a in actions:
        argv += ["--action", a]
    subprocess.run(argv, check=True, timeout=120)


def main():
    app, engine = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="supple-screensets-") as temporary:
        project = Path(temporary) / "song.json"
        subprocess.run([engine, "exec", '{"command":"track.add","args":{"name":"A"}}',
                        "--project", str(project), "--save", "--workdir", temporary],
                       check=True, stdout=subprocess.DEVNULL, timeout=60)
        ui_file = project.with_suffix(".ui.json")
        # Save the mixer open and zoomed in as "mixing", then the default as slot 1.
        ui_file.write_text(json.dumps({"arrange": {"pixels_per_second": 120.0}}))
        run_app(app, project, "ui.mixer", 'ui.screenset.save {"name": "mixing"}')
        ui = json.loads(ui_file.read_text(encoding="utf-8"))
        mixing = ui["screensets"]["mixing"]
        assert mixing["panels"]["mixer"] is True, mixing
        assert mixing["arrange"]["pixels_per_second"] == 120.0, mixing
        # Close the mixer and zoom out; recall "mixing": both come back.
        ui["panels"]["mixer"] = False
        ui["arrange"]["pixels_per_second"] = 20.0
        ui_file.write_text(json.dumps(ui))
        run_app(app, project, 'ui.screenset.load {"name": "mixing"}')
        ui = json.loads(ui_file.read_text(encoding="utf-8"))
        assert ui["panels"]["mixer"] is True, ui["panels"]
        assert ui["arrange"]["pixels_per_second"] == 120.0, ui["arrange"]
        # Slots are names; an unknown name fails the run.
        run_app(app, project, "ui.screenset.save:2")
        assert "2" in json.loads(ui_file.read_text(encoding="utf-8"))["screensets"]
        failed = subprocess.run(
            [app, str(project), "--offscreen", "--no-socket", "--frames", "2", "--audio", "none",
             "--action", 'ui.screenset.load {"name": "nope"}'], timeout=120,
            stderr=subprocess.PIPE, text=True)
        assert failed.returncode != 0 and "screenset.unknown" in failed.stderr, failed.stderr
        run_app(app, project, 'ui.screenset.delete {"name": "mixing"}')
        assert "mixing" not in json.loads(ui_file.read_text(encoding="utf-8"))["screensets"]
        # The side panel's state survives a run, the Browser tab's kind with it.
        # The file is written only when the state changed, so each run also
        # toggles the mixer.
        ui = json.loads(ui_file.read_text(encoding="utf-8"))
        ui["panels"]["side"] = {"visible": True, "tab": "browser", "width": 320.0, "browser": "files"}
        ui_file.write_text(json.dumps(ui))
        mixer_was = ui["panels"]["mixer"]
        run_app(app, project, "ui.mixer")
        panels = json.loads(ui_file.read_text(encoding="utf-8"))["panels"]
        assert panels["mixer"] is not mixer_was, panels
        assert panels["side"] == {"visible": True, "tab": "browser", "width": 320.0, "browser": "files"}, panels
        # A kind the Browser does not have is ignored: it opens on its first.
        ui["panels"]["side"]["browser"] = "nope"
        ui_file.write_text(json.dumps(ui))
        run_app(app, project, "ui.mixer")
        panels = json.loads(ui_file.read_text(encoding="utf-8"))["panels"]
        assert panels["mixer"] is not mixer_was, panels
        assert panels["side"]["browser"] == "plugins" and panels["side"]["tab"] == "browser", panels
    print("screensets: ok")


if __name__ == "__main__":
    main()
