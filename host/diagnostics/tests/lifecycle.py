#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
import json, os, pathlib, subprocess, sys, tempfile, signal
binary=sys.argv[1]
sanitized=len(sys.argv)>2 and bool(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="diagnostics-fixtures-") as folder:
    base=pathlib.Path(folder)
    for mode in ("normal","exit","abort","terminate","fault","thread-fault","stack","concurrent","teardown","echo","handled","killed") + (("failfast",) if os.name=="nt" else ()):
        root=base/mode/"app"/"diagnostics"
        args=[binary,str(root),mode]
        if mode=="echo": args.append('argument with spaces " and slash\\')
        run=subprocess.run(args,input=b"private-project-sentinel\n",stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
        found=list(root.glob("*.json"))
        if mode in ("normal","echo","handled"):
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
            assert report["kind"]==("crash" if report["cause"] else "unexpected_exit"),report
            if not sanitized and mode not in ("exit","killed","concurrent","failfast"):
                assert report["cause"]!=0,(mode,report)
                if mode=="terminate": assert report["cause"]==3,report
                if mode in ("fault","thread-fault","stack"): assert report["terminal_context"],(mode,report)
            if mode in ("killed","failfast"): assert not report["terminal_context"],report
            if sanitized: assert report["cause"]==0,(mode,report)
            assert "private-project-sentinel" not in raw
            assert str(root) not in raw
    if os.name != "nt":
        # The worker exits while its fork helper retains the channel. The
        # supervisor must finish before we release that helper. Timeout only
        # aborts a hung fixture; status/report contents decide correctness.
        root=base/"fork-channel"/"app"/"diagnostics"
        p=subprocess.Popen([binary,str(root),"fork-channel"],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        helper=int(p.stdout.readline())
        try:
            assert p.wait(timeout=30)==31
            assert len(list(root.glob("*.json")))==1
        finally:
            p.stdin.write(b"release");p.stdin.close()
            if p.poll() is None: p.kill();p.wait()
            p.stdout.close();p.stderr.close()
    if os.name != "nt":
        root=base/"ignored-sigchld"/"app"/"diagnostics"
        run=subprocess.run([binary,str(root),"normal"],capture_output=True,timeout=30,
                           preexec_fn=lambda:signal.signal(signal.SIGCHLD,signal.SIG_IGN))
        assert run.returncode==7,run
        assert not list(root.glob("*.json"))
        assert not list(root.glob("*.run"))
    # Invalid/partial bootstrap degrades to ordinary operation, with a watchdog
    # only to abort a hung fixture. No elapsed-time assertion.
    root=base/"bad-bootstrap"/"app"/"diagnostics"
    if os.name=="nt":
        bad=subprocess.run([binary,str(root),"normal","--diagnostic-worker","0:0:0:0"],capture_output=True,timeout=30)
        assert bad.returncode==7,bad
    else:
        import socket
        for prefix in (b"",b"partial"):
            parent,child=socket.socketpair()
            try:
                parent.sendall(prefix)
                bad=subprocess.Popen([binary,str(root),"normal","--diagnostic-worker","3"],
                    pass_fds=tuple({3,child.fileno()}),preexec_fn=lambda:os.dup2(child.fileno(),3),
                    stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                out,err=bad.communicate(timeout=30)
                assert bad.returncode==7,(bad.returncode,err)
            finally: parent.close();child.close()
    # A watchdog may abort these fixtures, but no elapsed-time threshold decides
    # a successful result. Both paths must reach EOF with collector already dead.
    for mode in ("collector-loss","fault-after-loss"):
        root=base/mode/"app"/"diagnostics"
        p=subprocess.Popen([binary,str(root),mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        worker=int(p.stdout.readline())
        try:
            p.kill();p.wait(timeout=30)
            p.stdin.write(b"worker survived\n");p.stdin.flush();p.stdin.close();p.stdin=None
            output,errors=p.communicate(timeout=30)
            if mode=="collector-loss": assert output==b"worker survived\n",output
            else: assert output==b"",output
        finally:
            p.stdout.close();p.stderr.close()

    if os.name!="nt":
        # Emulate a live collector withholding its reply. The native terminal
        # worker owns the only stdout writer, avoiding supervisor startup races.
        import socket
        parent,child=socket.socketpair()
        try:
            root=base/"withheld-ack"/"app"/"diagnostics"
            p=subprocess.Popen([binary,str(root),"terminal-no-ack",str(child.fileno())],
                pass_fds=(child.fileno(),),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            child.close()
            assert p.stdout.readline()==b"ready\n"
            out,err=p.communicate(b"fault now\n",timeout=30)
            assert p.returncode!=0,(out,err)
            capsule=parent.recv(128)
            if not sanitized: assert len(capsule)>8,capsule
        finally:
            parent.close();child.close()

print("diagnostics lifecycle: normal/error/teardown/stdio/collector loss passed")
