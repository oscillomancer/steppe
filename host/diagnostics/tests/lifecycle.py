#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
import json, os, pathlib, re, subprocess, sys, tempfile, signal
binary=sys.argv[1]
sanitized=len(sys.argv)>2 and bool(sys.argv[2])
resolver=sys.argv[3] if len(sys.argv)>3 and sys.argv[3] else None
def norm(path): return os.path.normcase(os.path.normpath(path))
def resolve_json(report_file,symbols=None):
    # The offline resolver (ADR-0083, topic 4) over a report this run produced, with the
    # fixture binary as the only place to look for symbols.
    run=subprocess.run([resolver,"--json","--symbols",str(symbols or binary),str(report_file)],capture_output=True,timeout=60)
    assert run.returncode==0,(run.returncode,run.stderr)
    out=json.loads(run.stdout)
    assert out["schema"]=="supple.crash-symbols/1.0",out
    return out
def fixture_module(resolved):
    matched=[m for m in resolved["modules"] if m.get("file") and norm(m["file"])==norm(binary)]
    assert len(matched)==1,(resolved["modules"],binary)
    return matched[0]
def function_names(thread): return [f.get("function","") for f in thread["frames"]]
with tempfile.TemporaryDirectory(prefix="diagnostics-fixtures-") as folder:
    base=pathlib.Path(folder)
    for mode in ("normal","negative-return","exit","abort","terminate","fault","thread-fault","stack","concurrent","teardown","echo","handled","killed") + (("failfast",) if os.name=="nt" else ()):
        root=base/mode/"app"/"diagnostics"
        args=[binary,str(root),mode]
        if mode=="echo": args.append('argument with spaces " and slash\\')
        run=subprocess.run(args,input=b"private-project-sentinel\n",stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
        found=list(root.glob("*.json"))
        if mode in ("normal","negative-return","echo","handled"):
            expected_code=7 if mode=="normal" else (0xffffffff if os.name=="nt" else 255) if mode=="negative-return" else 0
            assert run.returncode==expected_code,(mode,run)
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
                assert report["cause"]!=0,(mode,report,run.stderr)
                if mode=="terminate": assert report["cause"]==3,report
                if mode in ("fault","thread-fault","stack"): assert report["terminal_context"],(mode,report)
            if mode in ("killed","failfast"): assert not report["terminal_context"],report
            if mode in ("fault","thread-fault","terminate","abort") and not sanitized and sys.platform=="darwin":
                # macOS reads its own threads in the terminal handler (ADR-0083, topic 4):
                # the fault entry is walked, in an image dyld lists, with its Mach-O UUID.
                threads=report["threads"]
                assert threads and threads[0]["role"]=="fault" and threads[0]["state"]=="walked",(mode,report)
                assert len(threads[0]["frames"])>=2,(mode,threads[0])
                assert all(t["role"]!="fault" for t in threads[1:]),(mode,threads)
                assert [t["ordinal"] for t in threads]==list(range(len(threads)))
                assert report["modules"] and all(m["format"]=="macho" for m in report["modules"]),(mode,report["modules"])
                assert any(m["id"] and re.fullmatch("[0-9a-f]{32}",m["id"]) for m in report["modules"]),report["modules"]
                assert threads[0]["frames"][0].get("module") is not None,(mode,threads[0])
                for forbidden in ("/Users","\"tid\"","\"pid\"","registers"): assert forbidden not in raw,(mode,forbidden)
                for t in threads:
                    assert len(t["frames"])<=128
                    if t["state"]=="unavailable": assert t["reason"] in ("thread_exited","access_denied","registers_unreadable","deadline","unsupported"),t
                if resolver and mode=="fault":
                    resolved=resolve_json(found[0])
                    module=fixture_module(resolved)
                    assert module["state"]=="resolved" and module["functions"]>0,module
                    assert any(function_names(resolved["threads"][0])),resolved["threads"][0]
            if mode in ("terminate","abort") and not sanitized and (sys.platform.startswith("linux") or os.name=="nt"):
                # The calling thread is the fault entry and is read from the live thread
                # (terminate and abort capture no registers): its real stack, not a
                # one-frame fault entry with the stack filed under main.
                fault=report["threads"][0]
                assert fault["role"]=="fault" and fault["state"]=="walked",(mode,fault)
                assert len(fault["frames"])>=3,(mode,fault)
                assert all(t["role"]!="main" for t in report["threads"]),(mode,report["threads"])
            if sanitized: assert report["cause"]==0,(mode,report)
            assert "private-project-sentinel" not in raw
            assert str(root) not in raw
    root=base/"prior-terminate"/"app"/"diagnostics"
    prior=subprocess.run([binary,str(root),"prior-terminate"],capture_output=True,timeout=30)
    assert prior.returncode==44,prior
    assert b"prior terminate handler" in prior.stderr,prior
    prior_reports=list(root.glob("*.json"))
    assert len(prior_reports)==1
    prior_report=json.loads(prior_reports[0].read_text())
    assert prior_report["cause"]==(0 if sanitized else 3),prior_report
    if not sanitized: assert prior_report["kind"]=="crash",prior_report
    if os.name=="nt" and not sanitized:
        # A top-level filter already in place (the MSVC runtime always has one) must not
        # stop the last-chance filter from capturing, and must still run after it
        # (review round 29, F1). The fixture installs one before the frontend starts.
        root=base/"prior-filter"/"app"/"diagnostics"
        run=subprocess.run([binary,str(root),"prior-filter"],capture_output=True,timeout=30)
        assert run.returncode!=0,run
        assert b"last-chance filter installed: yes" in run.stderr,run.stderr
        assert b"prior filter ran" in run.stderr,run.stderr
        found=list(root.glob("*.json"))
        assert len(found)==1,(found,run.stderr)
        report=json.loads(found[0].read_text())
        assert report["kind"]=="crash" and report["terminal_context"] and report["cause"]!=0,report
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

    if sys.platform.startswith("linux") and not sanitized:
        # Thread traces (ADR-0083, topic 4). The fixture parks three threads in marked
        # functions and faults a fourth; it records where those functions are, and its
        # build ID read independently of the collector.
        def trace_run(mode):
            root=base/mode/"app"/"diagnostics"
            expected_file=base/(mode+"-expected.json")
            run=subprocess.run([binary,str(root),mode,str(expected_file)],capture_output=True,timeout=60)
            assert run.returncode!=0,(mode,run)
            expected=json.loads(expected_file.read_text())
            found=list(root.glob("*.json"))
            assert len(found)==1,(mode,found,run.stderr)
            raw=found[0].read_text()
            return expected,raw,json.loads(raw)
        def yama_scope():
            try: return int(pathlib.Path("/proc/sys/kernel/yama/ptrace_scope").read_text())
            except OSError: return 0
        expected,raw,report=trace_run("trace")
        assert report["kind"]=="crash" and report["terminal_context"],report
        threads=report["threads"]
        assert len(threads)==4,threads
        assert [t["role"] for t in threads]==["fault","main","other","other"],threads
        assert [t["ordinal"] for t in threads]==[0,1,2,3]
        assert not report["complete"]
        assert not re.search(r"\b%d\b"%expected["pid"],raw),"process id leaked"
        for forbidden in ("/proc","\"tid\"","\"pid\"","registers"):
            assert forbidden not in raw,forbidden
        if yama_scope()>=2 and os.geteuid()!=0:
            # Restricted tracing: partial but honest, never an empty success.
            for t in threads:
                assert t["state"] in ("walked","unavailable"),t
                if t["state"]=="unavailable": assert t["reason"]=="ptrace_denied",t
        else:
            assert all(t["state"]=="walked" for t in threads),threads
            exe=[i for i,m in enumerate(report["modules"]) if m["id"]==expected["build_id"]]
            assert len(exe)==1,(report["modules"],expected)
            def in_function(frame,name):
                return frame["module"]==exe[0] and int(frame["offset"],16)>=expected[name] and int(frame["offset"],16)<expected[name]+4096
            def index(frames,name):
                for i,f in enumerate(frames):
                    if in_function(f,name): return i
                return None
            fault=threads[0]["frames"]
            middle,outer=index(fault,"faultMiddle"),index(fault,"faultOuter")
            assert middle is not None and outer is not None and middle<outer,fault
            assert not fault[0]["return"] and all(f["return"] for f in fault[1:]),fault
            for t in threads[1:]:
                assert index(t["frames"],"parkOuter") is not None,t
            assert all(t["stop"] in ("end","not_executable","bad_frame_pointer","depth","unreadable","cycle") for t in threads),threads
            for t in threads:
                assert len(t["frames"])<=128
            if resolver:
                # Offline resolution names the marked functions from the binary's own symbols.
                report_file=list((base/"trace"/"app"/"diagnostics").glob("*.json"))[0]
                resolved=resolve_json(report_file)
                module=fixture_module(resolved)
                assert module["state"]=="resolved" and module["functions"]>0,module
                fault=" ".join(function_names(resolved["threads"][0]))
                assert "faultMiddle" in fault and "faultOuter" in fault and fault.index("faultMiddle")<fault.index("faultOuter"),fault
                assert "parkOuter" in " ".join(function_names(resolved["threads"][1])),resolved["threads"][1]
                for t in resolved["threads"]:
                    for f in t["frames"]:
                        if f["return"] and "function" in f: assert f["displacement"]>=1,f
                text=subprocess.run([resolver,"--symbols",binary,str(report_file)],capture_output=True,timeout=60)
                assert text.returncode==0 and b"faultOuter" in text.stdout and b"thread 0 fault: walked" in text.stdout,text
                # Without the build's files nothing is guessed: modules are not found, frames unnamed.
                empty=base/"no-symbols"; empty.mkdir()
                lost=resolve_json(report_file,empty)
                assert all(m["state"]=="not_found" for m in lost["modules"]),lost["modules"]
                assert not any("function" in f for t in lost["threads"] for f in t["frames"]),lost
                # Refusals: unreadable report, damaged report, no symbol location.
                assert subprocess.run([resolver,"--symbols",binary,str(base/"missing.json")],capture_output=True,timeout=60).returncode==1
                cut=base/"cut.json"; cut.write_text(report_file.read_text()[:200])
                bad=subprocess.run([resolver,"--symbols",binary,str(cut)],capture_output=True,timeout=60)
                assert bad.returncode==1 and bad.stderr,bad
                assert subprocess.run([resolver,str(report_file)],capture_output=True,timeout=60).returncode==2
        if yama_scope()<2 or os.geteuid()==0:
            # A thread in a killable kernel wait cannot be stopped: the collector gives
            # up at its deadline, and the supervisor must still see the worker exit and
            # write the report. The timeout is only a watchdog; the verdict is the report.
            root=base/"trace-killable"/"app"/"diagnostics"
            run=subprocess.run([binary,str(root),"trace-killable"],capture_output=True,timeout=30)
            assert run.returncode==128+signal.SIGSEGV,run
            found=list(root.glob("*.json"))
            assert len(found)==1,(found,run.stderr)
            assert not list(root.glob("*.run")),"run marker left behind"
            report=json.loads(found[0].read_text())
            assert report["kind"]=="crash" and report["terminal_context"],report
            assert report["native_fatal_status"] and report["code"]==signal.SIGSEGV,report
            by_role={}
            for t in report["threads"]: by_role.setdefault(t["role"],[]).append(t)
            assert by_role["fault"][0]["state"]=="walked",report["threads"]
            assert [t.get("reason") for t in report["threads"] if t["state"]=="unavailable"]==["deadline"],report["threads"]
        if os.geteuid()!=0:
            # A non-dumpable worker refuses every external read: all threads unavailable.
            expected,raw,report=trace_run("trace-nondumpable")
            assert report["kind"]=="crash",report
            assert len(report["threads"])>=1
            for t in report["threads"]:
                assert t["state"]=="unavailable" and t["reason"]=="maps_unreadable",t
                assert t["frames"]==[],t

    if os.name=="nt" and not sanitized:
        # Thread traces on Windows: the same fixture parks three threads and faults a
        # fourth. Offsets are attributed to the nearest marked function start below
        # them (functions are not page aligned here), and extra OS threads the process
        # may have are tolerated.
        root=base/"trace"/"app"/"diagnostics"
        expected_file=base/"trace-expected.json"
        run=subprocess.run([binary,str(root),"trace",str(expected_file)],capture_output=True,timeout=120)
        assert run.returncode!=0,run
        expected=json.loads(expected_file.read_text())
        found=list(root.glob("*.json"))
        assert len(found)==1,(found,run.stderr)
        raw=found[0].read_text()
        report=json.loads(raw)
        assert report["kind"]=="crash" and report["terminal_context"],report
        threads=report["threads"]
        assert len(threads)>=4,threads
        assert [t["role"] for t in threads[:2]]==["fault","main"],threads
        assert all(t["role"]=="other" for t in threads[2:]),threads
        assert [t["ordinal"] for t in threads]==list(range(len(threads)))
        assert not report["complete"]
        assert not re.search(r"\b%d\b"%expected["pid"],raw),"process id leaked"
        for forbidden in (".exe",".dll",".pdb","\\\\","C:","\"tid\"","\"pid\"","registers"):
            assert forbidden not in raw,forbidden
        assert expected["build_id"] and len(expected["build_id"])==40,expected
        exe=[i for i,m in enumerate(report["modules"]) if m["id"]==expected["build_id"]]
        assert len(exe)==1,(report["modules"],expected)
        assert all(m["format"]=="pe" for m in report["modules"]),report["modules"]
        if resolver:
            # The resolver matches the image by its CodeView id. Windows images give exported
            # names only (the PDB is not read), so a fixture with no exports has no names.
            resolved=resolve_json(found[0])
            assert fixture_module(resolved)["state"] in ("resolved","no_symbols"),resolved["modules"]
        names=("parkOuter","faultMiddle","faultOuter")
        def owner(frame):
            if frame["module"]!=exe[0]: return None
            off=int(frame["offset"],16)
            near=[(expected[n],n) for n in names if expected[n]<=off<expected[n]+512]
            return max(near)[1] if near else None
        def owners(thread): return [owner(f) for f in thread["frames"]]
        assert threads[0]["state"]=="walked",threads[0]
        assert "faultOuter" in owners(threads[0]),threads[0]
        assert not threads[0]["frames"][0]["return"] and all(f["return"] for f in threads[0]["frames"][1:]),threads[0]
        assert threads[1]["state"]=="walked" and "parkOuter" in owners(threads[1]),threads[1]
        assert sum(1 for t in threads[2:] if t["state"]=="walked" and "parkOuter" in owners(t))>=2,threads
        for t in threads:
            assert len(t["frames"])<=128
            if t["state"]=="unavailable": assert t["reason"] in ("access_denied","thread_exited","deadline","registers_unreadable"),t

print("diagnostics lifecycle: normal/error/teardown/stdio/collector loss passed")
