#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""B1/B2 capacity searches. Default devices are dry; real JACK needs exact-1 opt-in."""
import argparse
import base64
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("reaper_check", ROOT / "tools/reaper-check/check.py")
check = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = check
spec.loader.exec_module(check)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")
    temp.replace(path)


def command_output(command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT, timeout=3).strip()
    except (OSError, subprocess.SubprocessError) as error:
        return "unavailable: " + str(error)


def machine_facts(plugin_dir, plugin_file=None):
    governors = {}
    for path in sorted(Path("/sys/devices/system/cpu").glob("cpu*/cpufreq/scaling_governor")):
        try:
            governors[path.parent.parent.name] = path.read_text().strip()
        except OSError:
            governors[path.parent.parent.name] = "unavailable"
    cpu = "unavailable"
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            cpu = line.split(":", 1)[1].strip()
            break
    return {"kernel": platform.release(), "pipewire": command_output(["pipewire", "--version"]),
            "cpu": cpu, "governors": governors, "logical_cpus": os.cpu_count(),
            "revision": command_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"]),
            "engine_build": {},
            "dirty": bool(command_output(["git", "-C", str(ROOT), "status", "--porcelain"])),
            "plugins": {name: hashlib.sha256((plugin_dir / name).read_bytes()).hexdigest()
                        for name in ("Marram.clap", "Yarrow.clap", "Sundew.clap")} if plugin_file is None else
                       {str(plugin_file): hashlib.sha256(plugin_file.read_bytes()).hexdigest()}}


def zero_xruns(result):
    if not result.get("valid", False):
        raise check.CheckError("invalid measurement (rate/quantum, missing FX, processing error or stopped device)")
    if not result.get("requested_settings_granted", True):
        raise check.CheckError("requested rate/quantum was not granted; no capacity claim")
    return all(result.get(key) in (None, 0) for key in
               ("xruns", "media_xruns", "anticipation_misses", "process_errors",
                "synthetic_over_budget", "synthetic_late_wakes"))


def capacity_search(measure, maximum):
    """Doubling brackets then integer bisection; confirm independently, never invent an upper bound."""
    low, high = 0, 1
    while high <= maximum:
        if not zero_xruns(measure(high, False)):
            break
        low = high
        if high == maximum:
            break
        high = min(maximum, high * 2)
    capped = low == maximum
    while not capped and high - low > 1:
        mid = (low + high) // 2
        if zero_xruns(measure(mid, False)):
            low = mid
        else:
            high = mid
    confirmed = low > 0 and zero_xruns(measure(low, True))
    return {"tracks": low, "confirmed": confirmed, "capped": capped,
            "first_failing_bound": None if capped else high}


def markdown_row(report):
    search = report.get("search")
    run = report["runs"][-1]
    capacity = (">=" if search and search["capped"] else "") + str(search["tracks"]) if search else str(run["tracks"])
    fields = [report["timestamp"], report["host"], report["device"], report["anticipative"],
              capacity, search["confirmed"] if search else ("offline" if run.get("offline") else "not searched"), run.get("seconds", "n/a"),
              (run.get("xruns") if run.get("xruns") is not None else "n/a"),
              run.get("synthetic_over_budget", "n/a"), run.get("synthetic_late_wakes", "n/a"),
              run.get("media_xruns", "n/a" if run.get("offline") else 0),
              run.get("anticipation_misses", "n/a" if run.get("offline") else 0),
              run.get("sample_rate", 48000), run.get("quantum", "offline"), report["facts"]["kernel"],
              report["facts"]["pipewire"], (", ".join(sorted(set(report["facts"]["governors"].values()))) or "unavailable"),
              report["facts"]["revision"][:12], report["directory"]]
    return "| " + " | ".join(str(x).replace("|", "\\|").replace("\n", " / ") for x in fields) + " |\n"


def save_report(report, directory, markdown):
    write_json(directory / "report.json", report)
    if not report.get("runs"):
        return
    header = ("| UTC | host | device | anticipative | N | confirmed | seconds | audio xruns | synthetic over-budget | synthetic late wakes | media xruns | "
              "ahead misses | rate | granted quantum | kernel | PipeWire | governor | revision | artifacts |\n" +
              "|---|---|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|---|---|\n")
    previous = markdown.read_text() if markdown.exists() else ""
    text = previous + ("\n" + header if header not in previous else "") + markdown_row(report)
    markdown.parent.mkdir(parents=True, exist_ok=True)
    temp = markdown.with_name(markdown.name + ".tmp")
    temp.write_text(text)
    temp.replace(markdown)


def setup_script(session, folder, cfg):
    """Pure generator. REAPER creates/saves the RPP; public clap_chunk loads CLAP-owned state."""
    q = check.lua_string
    statements = []
    def chain(target, entries):
        lines = []
        for entry in entries:
            base64.b64decode(entry["state"], validate=True)
            lines += check.lua_scope([f"local fx = reaper.TrackFX_AddByName({target},{q('CLAP:' + entry['name'])},false,-1)",
                      "assert(fx >= 0, 'CLAP missing')",
                      f"local _,ident = reaper.TrackFX_GetNamedConfigParm({target},fx,'fx_ident')",
                      f"assert(ident:find({q(entry['plugin'])},1,true), 'unexpected CLAP identity')",
                      f"assert(reaper.TrackFX_SetNamedConfigParm({target},fx,'clap_chunk',{q(entry['state'])}), 'state rejected')",
                      f"local found,saved = reaper.TrackFX_GetNamedConfigParm({target},fx,'clap_chunk')",
                      f"assert(found and saved == {q(entry['state'])}, 'CLAP state readback mismatch')",
                      "loaded = loaded + 1"])
        return lines
    for index, track in enumerate(session["tracks"]):
        start = len(statements)
        statements += [f"reaper.InsertTrackAtIndex({index},true)", f"local track = reaper.GetTrack(0,{index})",
                       f"reaper.GetSetMediaTrackInfo_String(track,'P_NAME',{q(track['name'])},true)",
                       "reaper.SetMediaTrackInfo_Value(track,'I_NCHAN',2)",
                       f"reaper.SetMediaTrackInfo_Value(track,'D_VOL',{track['volume']:.17g})",
                       f"reaper.SetMediaTrackInfo_Value(track,'D_PANLAW',{session['pan_law']:.17g})"]
        if track["armed"]:
            statements += ["reaper.SetMediaTrackInfo_Value(track,'I_RECARM',1)",
                           "reaper.SetMediaTrackInfo_Value(track,'I_RECMON',1)",
                           "reaper.SetMediaTrackInfo_Value(track,'I_RECINPUT',1024)"]
        statements += chain("track", track["fx"])
        if track["notes"]:
            statements += [f"local item = reaper.CreateNewMIDIItemInProj(track,0,{session['loop_seconds']},false)",
                           "local take = assert(reaper.GetActiveTake(item))"]
            for note in track["notes"]:
                # Exported beat positions are converted using the same 120 BPM tempo map.
                statements += ["assert(reaper.MIDI_InsertNote(take,false,false," +
                    f"reaper.MIDI_GetPPQPosFromProjQN(take,{note['start']:.17g})," +
                    f"reaper.MIDI_GetPPQPosFromProjQN(take,{note['end']:.17g}),0,{note['key']},{note['velocity']},true))"]
            statements += ["reaper.MIDI_Sort(take)", "reaper.SetMediaItemInfo_Value(item,'D_FADEINLEN',0)",
                           "reaper.SetMediaItemInfo_Value(item,'D_FADEOUTLEN',0)"]
        statements[start:] = check.lua_scope(statements[start:])
    statements += chain("reaper.GetMasterTrack(0)", session["master"])
    body = "\n  ".join(statements)
    return f"""local folder,resource = {q(folder)}, {q(cfg)}
local log = assert(io.open(folder .. '/setup.tsv','w'))
local function say(k,v) log:write(k .. '\\t' .. tostring(v):gsub('[\\r\\n\\t]',' ') .. '\\n'); log:flush() end
local ok,err = xpcall(function()
  assert(reaper.GetResourcePath() == resource, 'private resource path mismatch')
  local opened,mode = reaper.GetAudioDeviceInfo('MODE')
  assert(not opened or mode:lower():find('dummy'), 'setup requires dummy audio')
  reaper.Audio_Quit()
  reaper.Main_OnCommand(40023,0)
  reaper.Main_SaveProjectEx(0,folder .. '/empty.RPP',8)
  reaper.GetSetProjectInfo(0,'PROJECT_SRATE',48000,true)
  reaper.GetSetProjectInfo(0,'PROJECT_SRATE_USE',1,true)
  reaper.SetCurrentBPM(0,120,true)
  local loaded = 0
  {body}
  reaper.GetSet_LoopTimeRange(true,true,0,{session['loop_seconds']},false)
  reaper.GetSetRepeat(1)
  reaper.GetSetProjectInfo_String(0,'RENDER_FILE',folder,true)
  reaper.GetSetProjectInfo_String(0,'RENDER_PATTERN','capacity',true)
  reaper.GetSetProjectInfo_String(0,'RENDER_FORMAT','ZXZhdxgAAA==',true)
  for key,value in pairs({{RENDER_SRATE=48000,RENDER_CHANNELS=2,RENDER_SETTINGS=0,
    RENDER_BOUNDSFLAG=0,RENDER_STARTPOS=0,RENDER_ENDPOS={session['loop_seconds']},
    RENDER_TAILFLAG=0,RENDER_NORMALIZE=0,RENDER_DITHER=0}}) do
    reaper.GetSetProjectInfo(0,key,value,true)
  end
  reaper.Main_SaveProjectEx(0,folder .. '/capacity.RPP',8)
  say('loaded_fx',loaded); say('version',reaper.GetAppVersion()); say('complete',1)
end,debug.traceback)
if not ok then say('error',err) end
log:close()
reaper.Main_openProject('noprompt:' .. folder .. '/empty.RPP')
reaper.defer(function() reaper.Main_OnCommand(40004,0) end)
"""


def measurement_script(folder, cfg, seconds, block=64):
    q = check.lua_string
    return f"""local folder,resource = {q(folder)}, {q(cfg)}
local log = assert(io.open(folder .. '/measurement.tsv','w'))
local function say(k,v) log:write(k .. '\\t' .. tostring(v):gsub('[\\r\\n\\t]',' ') .. '\\n'); log:flush() end
local done = false
local function finish(err)
  if done then return end
  done = true
  reaper.OnStopButton()
  reaper.Audio_Quit()
  if err then say('error',err) else say('complete',1) end
  log:close()
  reaper.Main_openProject('noprompt:' .. folder .. '/empty.RPP')
  reaper.defer(function() reaper.Main_OnCommand(40004,0) end)
end
local last_audio,last_media,started,baseline,deadline
local audio,media,polls,maxgap,previous = 0,0,0,0,0
local function poll()
  local ok,err = xpcall(function()
    local now = reaper.time_precise()
    if now >= baseline then
      local a,m = reaper.GetUnderrunTime()
      if not started then
        started,previous = now,now
        deadline = started + {seconds:.17g}
        last_audio,last_media = a,m
        say('baseline_audio_timestamp',a); say('baseline_media_timestamp',m)
      else
        if a ~= last_audio then audio = audio + 1; last_audio = a end
        if m ~= last_media then media = media + 1; last_media = m end
        polls = polls + 1; maxgap = math.max(maxgap,now-previous); previous = now
      end
      if now >= deadline then
        say('xruns',audio); say('media_xruns',media); say('seconds',now-started)
        say('last_audio_timestamp',last_audio); say('last_media_timestamp',last_media)
        say('polls',polls); say('max_poll_gap_seconds',maxgap)
        finish(); return
      end
    end
    assert(reaper.GetPlayState() & 1 == 1, 'transport stopped during measurement')
    reaper.defer(poll)
  end,debug.traceback)
  if not ok then finish(err) end
end
local ok,err = xpcall(function()
  assert(reaper.GetResourcePath() == resource, 'private resource mismatch')
  assert(os.getenv('SUPPLE_AUDIO_DEVICE_TEST') == '1', 'real audio not opted in')
  reaper.Main_openProject('noprompt:' .. folder .. '/capacity.RPP')
  reaper.Audio_Init()
  say('requested_block',{block})
  local opened,mode = reaper.GetAudioDeviceInfo('MODE')
  assert(opened and mode:lower():find('jack'), 'expected JACK audio')
  for _,key in ipairs({{'MODE','BSIZE','SRATE','IDENT_IN','IDENT_OUT'}}) do
    local found,value = reaper.GetAudioDeviceInfo(key); say(key,found and value or 'unavailable')
  end
  say('version',reaper.GetAppVersion())
  -- Play pressed right after Audio_Init is stopped again by the device's own
  -- startup (probed: state 1, then 0 within 20 ms); let it settle first.
  local settle = reaper.time_precise() + 1
  local function start()
    local ok2,err2 = xpcall(function()
      if reaper.time_precise() < settle then reaper.defer(start); return end
      reaper.GetSetRepeat(1)
      reaper.OnPlayButton()
      baseline = reaper.time_precise() + 1
      deadline = baseline + {seconds:.17g}
      reaper.defer(poll)
    end,debug.traceback)
    if not ok2 then finish(err2) end
  end
  reaper.defer(start)
end,debug.traceback)
if not ok then finish(err) end
"""


def export_session(args, directory, tracks):
    path = directory / "session.json"
    command = [str(args.engine), "--plugins", str(args.plugins), "--tracks", str(tracks),
               "--export-session", str(path), "--recipe", str(args.recipe)]
    subprocess.run(command, check=True, timeout=args.timeout)
    return json.loads(path.read_text()), command


def b2_run(args, directory, tracks):
    directory.mkdir(mode=0o700)
    session, export_command = export_session(args, directory, tracks)
    user = Path.home() / ".config/REAPER"
    before = check.snapshot(user)
    check.write_json(directory / "suite.config-before.json", before)
    commands = [export_command]
    try:
        ini = check.private_config(directory, args.plugins, user, block=args.block)
        runner = check.Runner(args.reaper, ini, user, directory, args.timeout)
        script = directory / "setup.lua"
        script.write_text(setup_script(session, directory, ini.parent))
        runner.run([str(script)], "setup")
        meta = check.read_metadata(directory / "setup.tsv")
        if meta.get("complete") != "1" or int(meta.get("loaded_fx", 0)) != (tracks + 1) * 3 + 1:
            raise check.CheckError(meta.get("error", "not every plugin loaded"))
        commands += runner.commands
        if args.device == "offline":
            runner.commands = []
            runner.run(["-renderproject", str(directory / "capacity.RPP")], "offline-render")
            # Reuse the existing WAV validator with this fixture's one-loop duration.
            previous = check.SECONDS
            try:
                check.SECONDS = session["loop_seconds"]
                samples = check.read_wav(directory / "capacity.wav")
            finally:
                check.SECONDS = previous
            if max(abs(x) for x in samples) < 1e-5:
                raise check.CheckError("offline capacity render is silent")
            commands += runner.commands
            return {"tracks": tracks, "valid": True, "offline": True, "loaded_fx": int(meta["loaded_fx"]),
                    "sample_rate": 48000, "seconds": session["loop_seconds"], "version": meta["version"],
                    "render": str(directory / "capacity.wav"), "commands": commands}
        # Separate fresh allowlisted real-device config; setup/render config remains dummy.
        live = directory / "live"
        live.mkdir(mode=0o700)
        live_ini = check.private_config(live, args.plugins, user, block=args.block, device="jack")
        runner = check.Runner(args.reaper, live_ini, user, directory, args.timeout + args.seconds, audio=True, block=args.block)
        script = directory / "measure.lua"
        script.write_text(measurement_script(directory, live_ini.parent, args.seconds, args.block))
        print(f"REAPER JACK benchmark: ordinary client request 48000/{args.block}; granted settings recorded.", flush=True)
        runner.run([str(script)], "measurement")
        meta = check.read_metadata(directory / "measurement.tsv", "measurement")
        if meta.get("complete") != "1":
            raise check.CheckError(meta.get("error", "measurement incomplete"))
        return {"tracks": tracks, "valid": True, "seconds": float(meta["seconds"]),
                "xruns": int(meta["xruns"]), "media_xruns": int(meta["media_xruns"]),
                "sample_rate": float(meta["SRATE"]), "quantum": int(meta["BSIZE"]),
                "requested_block": args.block,
                "requested_settings_granted": float(meta["SRATE"]) == 48000 and int(meta["BSIZE"]) == args.block,
                "version": meta["version"], "device_info": meta, "commands": commands + runner.commands}
    finally:
        after = check.snapshot(user)
        check.write_json(directory / "suite.config-after.json", after)
        if check.changes(before, after):
            raise check.ConfigChanged("USER CONFIG CHANGED during B2; see snapshot listings")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", choices=("b1", "b2"))
    parser.add_argument("--engine", type=Path, default=ROOT / "build/dev/host/engine/engine-bench")
    parser.add_argument("--plugins", type=Path)
    parser.add_argument("--block", type=int, choices=(64, 128), default=64)
    parser.add_argument("--plugin-file", type=Path, help="B1 only: installed third-party CLAP module")
    parser.add_argument("--plugin-id", help="descriptor ID within --plugin-file")
    parser.add_argument("--live", choices=("on", "off"), default="off")
    parser.add_argument("--fx-per-track", type=int, default=1)
    parser.add_argument("--workers", type=int, default=-1)
    parser.add_argument("--metrics", choices=("on", "off"), default="on")
    parser.add_argument("--sample-capacity", type=int, default=500000)
    parser.add_argument("--background", choices=("auto", "normal", "fifo"), default="auto",
                        help="B1 only: renderer and background pool scheduling")
    parser.add_argument("--background-priority", type=int, default=0)
    parser.add_argument("--callback-priority", type=int, default=0,
                        help="B1 paced only: SCHED_FIFO priority of the paced callback")
    parser.add_argument("--isolation", choices=("in_process", "shared_bridge", "dedicated"),
                        default="in_process", help="B1 only: every FX's isolation mode")
    parser.add_argument("--park-us", type=int, default=0,
                        help="B1 only, experimental: live helpers park after this many idle us (0: spin)")
    parser.add_argument("--recipe", type=Path, default=ROOT / "host/engine/bench/session.json")
    parser.add_argument("--device", choices=("paced", "offline", "jack"))
    parser.add_argument("--anticipative", choices=("on", "off", "both"), default="both")
    parser.add_argument("--search", action="store_true")
    parser.add_argument("--tracks", type=int, default=2)
    parser.add_argument("--max-tracks", type=int, default=256)
    parser.add_argument("--seconds", type=float, default=300)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--reaper", default=shutil.which("reaper"))
    args = parser.parse_args(argv)
    args.device = args.device or ("paced" if args.host == "b1" else "offline")
    if args.device == "jack" and os.environ.get("SUPPLE_AUDIO_DEVICE_TEST") != "1":
        parser.error("real audio requires SUPPLE_AUDIO_DEVICE_TEST=1 exactly")
    if (args.host == "b1" and args.device == "offline") or (args.host == "b2" and args.device == "paced"):
        parser.error("B1 uses paced/jack; B2 uses offline/jack")
    if args.host == "b2" and (not args.reaper or (args.device == "offline" and args.search)):
        parser.error("B2 needs REAPER; offline mode checks a fixed N, not capacity")
    if not all(math.isfinite(v) and v > 0 for v in (args.seconds, args.timeout)) or not (1 <= args.tracks <= 4096 and 1 <= args.max_tracks <= 4096):
        parser.error("positive finite durations and N in 1..4096 required")
    if args.plugin_file and (args.host != "b1" or not args.plugin_id):
        parser.error("--plugin-file requires B1 and --plugin-id")
    if not args.plugin_file and (args.plugin_id or args.live != "off" or args.fx_per_track != 1):
        parser.error("effect options require --plugin-file")
    if args.plugin_file:
        args.plugin_file = args.plugin_file.resolve()
    args.engine = args.engine.resolve()
    args.plugins = (args.plugins or args.engine.parents[2] / "plugins/clap").resolve()
    args.report = (args.report or ROOT / f"docs/benchmarks/{args.host}.md").resolve()
    directory = (args.output_dir or ROOT / "build/bench" / (time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + uuid.uuid4().hex[:8])).resolve()
    directory.mkdir(parents=True, mode=0o700, exist_ok=False)
    if args.device == "jack":
        print(f"Real-device benchmark: client request 48000/{args.block}; no forced quantum/rate.", flush=True)
    modes = [True, False] if args.anticipative == "both" else [args.anticipative == "on"]
    if args.host == "b2":
        modes = ["REAPER default"]
    status = 0
    for mode in modes:
        report = {"schema": "supple.capacity-report/1.0", "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                  "host": args.host, "device": args.device, "anticipative": mode,
                  "requested_seconds": args.seconds, "directory": str(directory), "runs": []}
        mode_dir = directory / str(mode).replace(" ", "-")
        mode_dir.mkdir()
        try:
            report["facts"] = machine_facts(args.plugins, args.plugin_file)
            cache = args.engine.parents[2] / "CMakeCache.txt"
            if cache.exists():
                report["facts"]["engine_build"] = {line.split(":", 1)[0]: line.split("=", 1)[1]
                    for line in cache.read_text().splitlines() if "=" in line and
                    line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS"))}
            report["facts"]["engine_sha256"] = hashlib.sha256(args.engine.read_bytes()).hexdigest()
            report["facts"]["recipe_sha256"] = hashlib.sha256(args.recipe.read_bytes()).hexdigest()
            effect_args = []
            if args.plugin_file:
                source = mode_dir / "input.wav"
                check.test_signal(source)
                report["facts"]["input_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
                effect_args = ["--plugin-file", str(args.plugin_file), "--plugin-id", args.plugin_id,
                               "--input", str(source), "--live", args.live, "--fx-per-track", str(args.fx_per_track)]
            def measure(n, confirmation):
                run_dir = mode_dir / f"run-{len(report['runs'])+1}-n{n}"
                if args.host == "b1":
                    run_dir.mkdir()
                    output = run_dir / "run.json"
                    command = [str(args.engine), "--plugins", str(args.plugins), "--device", args.device,
                               "--tracks", str(n), "--seconds", str(args.seconds), "--anticipative", "on" if mode else "off",
                               "--output", str(output), "--recipe", str(args.recipe), "--block", str(args.block),
                               "--workers", str(args.workers), "--metrics", args.metrics,
                               "--sample-capacity", str(args.sample_capacity),
                               "--background", args.background,
                               "--background-priority", str(args.background_priority),
                               "--callback-priority", str(args.callback_priority),
                               "--isolation", args.isolation, "--park-us", str(args.park_us),
                               *effect_args]
                    with (run_dir / "console.log").open("w") as log:
                        completed = subprocess.run(command, check=False, timeout=args.seconds + args.timeout,
                                                   stdout=log, stderr=subprocess.STDOUT)
                    if not output.exists():
                        completed.check_returncode()
                    result = json.loads(output.read_text())
                    result["commands"] = [command]
                else:
                    result = b2_run(args, run_dir, n)
                result["confirmation"] = confirmation
                report["runs"].append(result)
                write_json(mode_dir / "progress.json", report)
                print(f"{args.host} {mode}: N={n}, xruns={result.get('xruns','offline')}, "
                      f"ahead={result.get('anticipation_misses',0)}, confirmation={confirmation}", flush=True)
                return result
            if args.search:
                report["search"] = capacity_search(measure, args.max_tracks)
                if not report["search"]["confirmed"]:
                    status = 1
            else:
                if not measure(args.tracks, False).get("valid"):
                    status = 1
        except (OSError, ValueError, subprocess.SubprocessError, check.CheckError) as error:
            report["error"] = {"code": "bench.run", "message": str(error), "path": str(mode_dir),
                               "hint": "inspect retained logs; run on an idle machine with the requested build"}
            status = 1
            print(str(error), file=sys.stderr)
        save_report(report, mode_dir, args.report)
        print(mode_dir / "report.json", flush=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
