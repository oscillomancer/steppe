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
print("host inventory passed")
