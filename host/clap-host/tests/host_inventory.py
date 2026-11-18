#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# The plug-in inventory of a crash report (ADR-0083, topic 5c), from the real host: the fixture
# program loads plug-ins through host::Instance under the diagnostics frontend and faults.
import json, os, pathlib, subprocess, sys, tempfile
binary=sys.argv[1]
sanitized=len(sys.argv)>2 and bool(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="host-inventory-") as folder:
    root=pathlib.Path(folder)/"app"/"diagnostics"
    run=subprocess.run([binary,str(root),"loaded"],capture_output=True,timeout=120)
    assert run.returncode!=0,run
    found=list(root.glob("*.json"))
    assert len(found)==1,(found,run.stderr)
    raw=found[0].read_text()
    report=json.loads(raw)
    plugins=report["plugins"]
    live=[p for p in plugins if p["state"]!="destroyed"]
    kinds=sorted((p["kind"],p["state"],p.get("plugin_id")) for p in live)
    # The library both instances came from, and the two instances alive at the fault: one
    # activated, one only created. Every other instance (the scan's probes, the one made and
    # dropped) is listed as destroyed.
    assert kinds==[("clap","active","com.suppleaudio.test.gain"),("clap","inactive","com.suppleaudio.test.crash"),("clap_library","active",None)],kinds
    assert any(p["state"]=="destroyed" for p in plugins),plugins
    for p in plugins:
        assert p["origin"]=="foreign" and p["mode"]=="in_process",p
        if p["kind"]=="clap": assert p.get("name") and p.get("vendor"),p
    assert [p["id"] for p in plugins]==sorted(p["id"] for p in plugins),plugins
    assert not any("inventory messages" in u for u in report["unavailable"]),report["unavailable"]
    # The fault is in the program, not in a plug-in's binary, and no thread is inside a plug-in
    # entry: the report names no plug-in, and everything it left out is counted by kind.
    assert report["attribution"]=={"confidence":"unknown","plugin":None},report["attribution"]
    assert report["dropped"]["messages"]==0 and report["dropped"]["plugins"]==0,report["dropped"]
    # The module each instance came from is a module of the report that is not the program's.
    threads=report["threads"]
    if not sanitized and threads and threads[0].get("state")=="walked" and sys.platform!="win32":
        module=threads[0]["frames"][0].get("module")
        ids={m["id"] for m in report["modules"] if m["id"]}
        for p in live:
            assert "build" in p,p
            assert p["build"]["id"] in ids,(p,ids)
            if module is not None: assert p["build"]["id"]!=report["modules"][module]["id"],p
    # No path of the plug-in, and no address, reaches the report.
    assert "test-fixtures" not in raw and "\"image\"" not in raw and str(pathlib.Path(binary).parent) not in raw

# Bridge processes (ADR-0083, topic 6a), from the real host and the real bridge. The fixture
# program bridges the crash plug-in and the host observes each bridge's end.
def run_bridged(mode,*more):
    with tempfile.TemporaryDirectory(prefix="host-bridges-") as folder:
        root=pathlib.Path(folder)/"app"/"diagnostics"
        run=subprocess.run([binary,str(root),mode,*more],capture_output=True,timeout=180)
        assert run.returncode!=0,run  # the program itself faults at the end
        reports=[json.loads(p.read_text()) for p in sorted(root.glob("*.json"))]
        leftovers=[p.name for p in root.iterdir() if p.suffix in (".run",".pub")]
        assert not leftovers,leftovers
        return reports,run

# A bridge the host kills is a `host` end and gets no report of its own; the program's own report
# still lists it.
reports,run=run_bridged("bridged-hang")
assert len(reports)==1,(len(reports),run.stderr)
crash=reports[0]
assert crash["kind"] in (("crash","unexpected_exit") if sanitized else ("crash",)) and "bridge" not in crash,crash["kind"]
assert len(crash["bridges"])==1,crash["bridges"]
bridge=crash["bridges"][0]
assert bridge["state"]=="ended" and bridge["ended"]["how"]=="host",bridge
member=[p for p in crash["plugins"] if p.get("bridge")==bridge["id"]]
assert [p["plugin_id"] for p in member]==["com.suppleaudio.test.crash"] and member[0]["mode"]=="bridged",member
assert bridge["instances"]==[member[0]["id"]],(bridge,member)
assert crash["dropped"]["bridges"]==0 and crash["dropped"]["bridge_reports"]==0,crash["dropped"]

if not sanitized:
    # A bridge that dies by itself while the program lives gets a report of its own, once: the
    # four deaths after the first repeat it, and are only counted.
    reports,run=run_bridged("bridged-crash")
    assert len(reports)==2,(len(reports),run.stderr)
    exit_report=[r for r in reports if r["kind"]=="bridge_exit"]
    program=[r for r in reports if r["kind"]=="crash"]
    assert len(exit_report)==1 and len(program)==1,[r["kind"] for r in reports]
    e=exit_report[0]
    assert e["incident"]!=program[0]["incident"],e["incident"]
    assert e["bridge"]==1 and e["complete"] is False and e["terminal_context"] is False,e
    assert e["commands"]==[],e
    # Topic 6c: the bridge's own capsule is in it, and says whose account it is. The fault thread
    # is the plug-in's audio thread inside the bridge, walked; the bridge's command thread is
    # listed beside it by role, not collected.
    assert e["capture"]=="bridge",e
    threads=e["threads"]
    assert threads and threads[0]["role"]=="fault" and threads[0]["state"]=="walked",threads
    assert threads[0]["registered"]=="bridge_audio" and threads[0]["entry"]=="process",threads[0]
    # Linux and Windows capture the fault thread only and list the others from the table
    # (`not_collected`); macOS walks every thread of the task, so there the control thread is walked.
    others="walked" if sys.platform=="darwin" else "unavailable"
    assert any(t["role"]=="bridge_control" and t["state"]==others and (others=="walked" or t["reason"]=="not_collected") for t in threads[1:]),threads
    ids=[m["id"] for m in e["modules"]]
    assert ids and all(i is None or len(i)%2==0 for i in ids),ids
    ended=[b for b in e["bridges"] if b["id"]==e["bridge"]][0]
    assert ended["state"]=="ended" and ended["ended"]["how"] in ("signal","exception"),ended
    assert ended["ended"]["code"]==e["code"],(ended,e["code"])
    member=[p for p in e["plugins"] if p.get("bridge")==e["bridge"]]
    assert [p["plugin_id"] for p in member]==["com.suppleaudio.test.crash"],member
    # The fault was in the plug-in's own binary, whose build the bridge recorded when it created
    # the plug-in: the report says so with the evidence of its own capsule.
    assert e["attribution"]=={"confidence":"fault_in_image","plugin":member[0]["id"]},e["attribution"]
    build=member[0].get("build")
    assert build and build["id"] in ids,(build,ids)
    assert threads[0]["frames"][0]["module"]==ids.index(build["id"]),(threads[0]["frames"][0],ids)
    assert member[0]["executing"]==[{"entry":"process","thread":0,"match":"thread"}],member[0]
    assert any("written by the bridge process itself" in u for u in e["unavailable"]),e["unavailable"]
    assert "\"image\"" not in json.dumps(e) and "0x" not in json.dumps(e["plugins"]),e
    # The program's own report knows all five bridges, and that four reports were not written.
    crash=program[0]
    assert [b["id"] for b in crash["bridges"]]==[1,2,3,4,5],crash["bridges"]
    assert all(b["state"]=="ended" and b["ended"]["how"] in ("signal","exception") for b in crash["bridges"]),crash["bridges"]
    assert crash["dropped"]["bridge_reports"]==4 and crash["dropped"]["bridges"]==0,crash["dropped"]
    assert any("not reported separately" in u for u in crash["unavailable"]),crash["unavailable"]
    assert "bridge" not in crash and crash["attribution"]["confidence"]!="bridge_exit",crash["attribution"]
    # Topic 6c, from the real host: each bridge's own capture reached the supervisor through the
    # slot the host handed it (a capsule the supervisor read after the bridge's end), and the one
    # report above is made from it. The fixture's hook inside the supervisor says what it found
    # for every one of the five.
    slots=[l for l in run.stderr.decode(errors="replace").splitlines() if l.startswith("slot bridge=")]
    assert [l.split()[2] for l in slots]==["capsule=ok"]*5,(slots,run.stderr)
    assert [l.split()[4] for l in slots]==["parsed=yes"]*5,(slots,run.stderr)  # and the parser takes it
    # Topic 6c.5: the same bridge, ended in every way a plug-in can end it, each from its own
    # fixture program. Each says the bridge wrote the account, walks the fault thread, and names
    # the plug-in that was in it. What differs is where the fault was: in the plug-in's binary
    # (the instruction is in the build the bridge recorded), or in the system's code under the
    # plug-in's entry (abort, a raised signal, std::terminate), which the open entry still names.
    posix=sys.platform!="win32"
    # A bad address is SIGSEGV or, on some systems, SIGBUS; on Windows it is STATUS_ACCESS_VIOLATION,
    # which a report carries as the signed 32-bit value of 0xC0000005.
    faults=(10,11) if posix else (-1073741819,)
    # A frame is a module and an offset into it, never an address: an absolute address would give
    # the load address away in a report the user may pass on (the spec's one rule about frames).
    # A load address is at least 2**40 on Linux and Windows, an offset below 2**32 there; on macOS
    # an executable's link address starts at 2**32, so only the plug-in's own dylib (linked at 0)
    # is held to it there.
    def frame_offsets_are_offsets(e):
        member=[p for p in e["plugins"] if p.get("bridge")==e["bridge"] and p.get("build")]
        ids=[m["id"] for m in e["modules"]]
        plugin=ids.index(member[0]["build"]["id"]) if member and member[0]["build"]["id"] in ids else None
        for t in e["threads"]:
            for f in t.get("frames",[]):
                if f.get("module") is None: continue
                offset=int(f["offset"],16)
                if sys.platform!="darwin" or f["module"]==plugin:
                    assert offset<2**32,("a frame offset is an address",f,t["role"])
    def bridge_report(*args):
        reports,run=run_bridged(*args)
        exits=[r for r in reports if r["kind"]=="bridge_exit"]
        assert len(exits)==1 and len([r for r in reports if r["kind"]=="crash"])==1,(args,[r["kind"] for r in reports],run.stderr)
        e=exits[0]
        assert e.get("capture")=="bridge" and e["complete"] is False and e["commands"]==[],e
        ended=[b for b in e["bridges"] if b["id"]==e["bridge"]][0]["ended"]
        assert ended["how"] in ("signal","exception") and ended["code"]==e["code"],(ended,e["code"])
        slots=[l for l in run.stderr.decode(errors="replace").splitlines() if l.startswith("slot bridge=")]
        assert [l.split()[2:5:2] for l in slots]==[["capsule=ok","parsed=yes"]],(slots,run.stderr)
        t=e["threads"]
        assert t and t[0]["role"]=="fault" and t[0]["state"]=="walked" and t[0]["frames"],t
        assert "\"image\"" not in json.dumps(e) and "0x" not in json.dumps(e["plugins"]),e
        frame_offsets_are_offsets(e)
        return e,ended
    def named(e):
        member=[p for p in e["plugins"] if p.get("bridge")==e["bridge"] and p["plugin_id"]=="com.suppleaudio.test.crash"]
        assert len(member)==1,e["plugins"]
        return member[0]
    def in_build(e,member):
        ids=[m["id"] for m in e["modules"]]
        assert member["build"]["id"] in ids,(member,ids)
        assert e["threads"][0]["frames"][0]["module"]==ids.index(member["build"]["id"]),(e["threads"][0]["frames"][0],ids)
    def counted(e,member):
        # An entry the table held open that no thread could be given is said so, never dropped
        # in silence (a stack overflow's stack pointer is below the stack the thread recorded).
        assert member.get("executing") or e["dropped"]["cells"]>=1,(member,e["dropped"])
    # (mode, level, the registered role of the fault thread or None, the entry it was in, codes)
    scenarios=[(4,"fault_in_image","bridge_audio","process",faults),
               (8,"fault_in_image",None,None,faults),
               (5,"fault_in_image","bridge_control","destroy",faults)]
    if posix:
        # raise(SIGSEGV) and abort() end a Windows bridge with exit code 3, which is no crash to
        # the host there; a stack overflow and std::terminate are not yet shown on Windows.
        scenarios+=[(1,"active_token","bridge_audio","process",(11,)),
                    (6,"active_token","bridge_audio","process",(6,)),
                    (7,"fault_in_image","bridge_audio",None,faults),
                    (9,"active_token","bridge_audio","process",(6,))]
    for mode,level,registered,entry,codes in scenarios:
        e,ended=bridge_report("bridged-one",str(mode))
        member=named(e)
        label=(mode,e["attribution"],e["threads"][0],ended)
        assert e["attribution"]=={"confidence":level,"plugin":member["id"]},label
        assert e["code"] in codes and ended["how"]==("signal" if posix else "exception"),label
        assert e["threads"][0].get("registered")==registered,label
        if entry: assert e["threads"][0].get("entry")==entry,label
        if level=="fault_in_image": in_build(e,member)
        if mode!=7: assert member["executing"] and member["executing"][0]["entry"]==(entry or "process"),label
        counted(e,member)
        if mode==8:
            # The plug-in's own thread is not one the bridge registered; the audio thread that
            # was waiting on it is listed beside it, inside the entry it was in.
            assert member["executing"]==[{"entry":"process","thread":member["executing"][0]["thread"],"match":"other_thread"}],member
    # Two plug-ins of one binary share a bridge, and the one that faults is named, not the other.
    e,ended=bridge_report("bridged-shared")
    crash=named(e)
    others=[p for p in e["plugins"] if p.get("bridge")==e["bridge"] and p is not crash]
    assert len(others)==1 and others[0]["plugin_id"]=="com.suppleaudio.test.gain" and others[0]["build"]==crash["build"],e["plugins"]
    assert e["attribution"]=={"confidence":"fault_in_image","plugin":crash["id"]},e["attribution"]
    assert not others[0].get("executing") and crash["executing"][0]["entry"]=="process",e["plugins"]
    in_build(e,crash)
    # The supervisor writes every bridge report before the worker's own, though the writer is slow
    # (a disk, a retention pass): the fixture holds the bridge report's write open until the
    # supervisor reaches the worker's report, if it ever does before the hold's bound, and says
    # each step on stderr. The program faults before the bridge's report is awaited.
    reports,run=run_bridged("bridged-slow-writer")
    steps=[l.split()[1] for l in run.stderr.decode(errors="replace").splitlines() if l.startswith("step ")]
    assert steps==["bridge-write","bridge-written","worker-report"],(steps,run.stderr)
    assert sorted(r["kind"] for r in reports)==["bridge_exit","crash"],[r["kind"] for r in reports]
print("host inventory passed")
