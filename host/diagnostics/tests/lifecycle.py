#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
import json, os, pathlib, subprocess, sys, tempfile, signal
binary=sys.argv[1]
with tempfile.TemporaryDirectory(prefix="diagnostics-fixtures-") as folder:
    base=pathlib.Path(folder)
    for mode in ("normal","exit","abort","teardown","echo"):
        root=base/mode/"app"/"diagnostics"
        args=[binary,str(root),mode]
        if mode=="echo": args.append('argument with spaces " and slash\\')
        run=subprocess.run(args,input=b"private-project-sentinel\n",stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
        found=list(root.glob("*.json"))
        if mode in ("normal","echo"):
            assert run.returncode==(7 if mode=="normal" else 0),(mode,run)
            assert not found,(mode,found)
            assert not list(root.glob("*.run")),mode
            if mode=="echo": assert run.stdout==b"private-project-sentinel\n",run
        else:
            assert run.returncode!=0,(mode,run)
            assert len(found)==1,(mode,found,run.stderr)
            raw=found[0].read_text();report=json.loads(raw)
            assert report["schema"]=="supple.crash-report/1.0"
            assert not report["complete"]
            assert report["kind"]==("unexpected_exit" if mode=="exit" else "crash")
            assert "private-project-sentinel" not in raw
            assert str(root) not in raw
    # A dead collector must neither terminate nor hold a healthy worker's stdio.
    root=base/"loss"/"app"/"diagnostics"
    p=subprocess.Popen([binary,str(root),"collector-loss"],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    worker=int(p.stdout.readline())
    try:
        p.kill();p.wait(timeout=30)
        p.stdin.write(b"worker survived\n");p.stdin.flush();p.stdin.close()
        assert p.stdout.readline()==b"worker survived\n"
        assert p.stdout.read()==b"" # no supervisor copy keeps EOF open
    finally:
        p.stdout.close();p.stderr.close()
print("diagnostics lifecycle: normal/error/teardown/stdio/collector loss passed")
