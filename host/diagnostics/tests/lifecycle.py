#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
import contextlib, json, os, pathlib, re, subprocess, sys, tempfile, signal, threading, traceback
binary=sys.argv[1]
sanitized=len(sys.argv)>2 and bool(sys.argv[2])
resolver=sys.argv[3] if len(sys.argv)>3 and sys.argv[3] else None
# Every section runs even if an earlier one failed, and all failures are reported at the
# end: a native run on a platform nobody can try locally should answer as many questions
# as it can.
failures=[]
@contextlib.contextmanager
def check(name):
    try: yield
    except Exception:
        failures.append(name)
        print("FAILED: "+name,file=sys.stderr)
        traceback.print_exc()
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
        with check(f"mode {mode}"):
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
                assert not list(root.glob("*.pub")),mode
                if mode=="echo": assert run.stdout==b"private-project-sentinel\n",run
            else:
                assert run.returncode!=0,(mode,run)
                assert len(found)==1,(mode,found,run.stderr)
                assert not list(root.glob("*.pub")),(mode,"supervisor hold left behind")
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
                    assert len(fault["frames"])>=3,(mode,fault,run.stderr)
                    assert all(t["role"] not in ("main","model") for t in report["threads"]),(mode,report["threads"])
                if sanitized: assert report["cause"]==0,(mode,report)
                assert "private-project-sentinel" not in raw
                assert str(root) not in raw
    # The program's own terminate handler runs after the capture, whether std::terminate was
    # called directly or an exception nobody caught led to it (on Windows that exception
    # first meets the top-level filter, which must leave it to the runtime: review round 31, F1).
    for mode in ("prior-terminate","prior-terminate-throw"):
        with check(f"{mode}"):
            root=base/mode/"app"/"diagnostics"
            prior=subprocess.run([binary,str(root),mode],capture_output=True,timeout=30)
            assert prior.returncode==44,(mode,prior)
            assert b"prior terminate handler" in prior.stderr,(mode,prior)
            prior_reports=list(root.glob("*.json"))
            assert len(prior_reports)==1,(mode,prior_reports)
            prior_report=json.loads(prior_reports[0].read_text())
            assert prior_report["cause"]==(0 if sanitized else 3),(mode,prior_report)
            if not sanitized: assert prior_report["kind"]=="crash",(mode,prior_report)
    # An exception out of a thread's body (a std::thread or a raw one) or a broken noexcept on
    # it reaches std::terminate on that thread, and the report says terminate (cause 3) on
    # every platform. The MSVC runtime keeps the terminate handler per thread (review round
    # 35, F1): a thread that registered with the diagnostics gets ours and must say 3 there
    # too; one that never registered may say 3 or, if the handler is per thread and missing,
    # 4 (abort), which the run prints so the answer is on record.
    for mode in ("thread-throw","raw-thread-throw","thread-throw-registered","raw-thread-throw-registered"):
        with check(mode):
            root=base/mode/"app"/"diagnostics"
            thrown=subprocess.run([binary,str(root),mode],capture_output=True,timeout=30)
            assert thrown.returncode!=0,(mode,thrown)
            thrown_reports=list(root.glob("*.json"))
            assert len(thrown_reports)==1,(mode,thrown_reports,thrown.stderr)
            thrown_report=json.loads(thrown_reports[0].read_text())
            if sanitized: assert thrown_report["cause"]==0,(mode,thrown_report)
            else:
                assert thrown_report["kind"]=="crash",(mode,thrown_report)
                unregistered_on_windows=os.name=="nt" and not mode.endswith("registered")
                allowed=(3,4) if unregistered_on_windows else (3,)
                assert thrown_report["cause"] in allowed,(mode,thrown_report)
                if unregistered_on_windows:
                    print("%s: terminate hook on a thread that never registered: cause %d (%s)"%(mode,thrown_report["cause"],"per-process" if thrown_report["cause"]==3 else "per-thread"),flush=True)
    if os.name=="nt" and not sanitized:
        with check("prior-filter"):
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
            assert report["kind"]=="crash" and report["terminal_context"] and report["cause"]==2,report
        with check("prior-filter-resume"):
            # A chained filter that asks to resume the faulting code is not obeyed: the fault
            # stays fatal and the code after it never runs (review round 31, F2).
            root=base/"prior-filter-resume"/"app"/"diagnostics"
            run=subprocess.run([binary,str(root),"prior-filter-resume"],capture_output=True,timeout=30)
            assert run.returncode!=0,run
            assert b"prior filter ran" in run.stderr,run.stderr
            assert b"resumed after the fault" not in run.stderr,run.stderr
            found=list(root.glob("*.json"))
            assert len(found)==1,(found,run.stderr)
            report=json.loads(found[0].read_text())
            assert report["kind"]=="crash" and report["terminal_context"] and report["cause"]==2,report
        with check("prior-filter-cpp-code"):
            # The code the runtime gives a C++ exception, raised by hand: the prior filter
            # (the fixture's, which does not turn it into std::terminate) returns, so nobody
            # is handling it and it is captured as a fault, not lost (review round 35, F2).
            root=base/"prior-filter-cpp-code"/"app"/"diagnostics"
            run=subprocess.run([binary,str(root),"prior-filter-cpp-code"],capture_output=True,timeout=30)
            assert run.returncode!=0,run
            assert b"prior filter ran" in run.stderr,run.stderr
            found=list(root.glob("*.json"))
            assert len(found)==1,(found,run.stderr)
            report=json.loads(found[0].read_text())
            assert report["kind"]=="crash" and report["terminal_context"] and report["cause"]==2,report
    with check('fork-channel'):
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
    with check('ignored-sigchld'):
        if os.name != "nt":
            root=base/"ignored-sigchld"/"app"/"diagnostics"
            run=subprocess.run([binary,str(root),"normal"],capture_output=True,timeout=30,
                               preexec_fn=lambda:signal.signal(signal.SIGCHLD,signal.SIG_IGN))
            assert run.returncode==7,run
            assert not list(root.glob("*.json"))
            assert not list(root.glob("*.run"))
            assert not list(root.glob("*.pub"))
    with check('bad-bootstrap'):
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
    # The worker's watch on its supervisor (ADR-0091). Each section is decided by what
    # happened (what the worker concluded, whether it stopped, whether it is gone), never by
    # how long anything took; the 30-second limits only end a run that hangs.
    def worker_gone(pid):
        # POSIX only (Windows has the end of the worker's output, which closes with it). The
        # worker is an orphan by now and init reaps it, a moment after its output closes; the
        # limit only ends a run where it never goes.
        if os.name=="nt": return True
        import time
        for _ in range(3000):
            try: os.kill(pid,0)
            except ProcessLookupError: return True
            except PermissionError: return False
            time.sleep(0.01)
        return False
    @contextlib.contextmanager
    def watchdog(p,worker):
        # Ends a run whose worker hangs: reads of its output then return and the checks fail.
        def end():
            if os.name!="nt":
                with contextlib.suppress(ProcessLookupError): os.kill(worker,signal.SIGKILL)
            with contextlib.suppress(Exception): p.kill()
        timer=threading.Timer(30,end);timer.start()
        try: yield
        finally: timer.cancel()
    def read_until_eof(p,worker):
        try: return p.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            if os.name!="nt":
                with contextlib.suppress(ProcessLookupError): os.kill(worker,signal.SIGKILL)
            p.kill(); p.communicate()
            raise AssertionError("the worker did not end")
    def spawn(mode):
        root=base/mode/"app"/"diagnostics"
        p=subprocess.Popen([binary,str(root),mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        return p,int(p.stdout.readline())
    with check('collector-loss'):
        # The supervisor crashes by itself (its own fatal handler says "collector failed"
        # first): the worker must go on running and serving, with no collector. The old
        # reading of this, a supervisor killed from outside, is the next section. Sanitizer
        # builds keep their own fatal handlers, so the supervisor there says nothing.
        if not sanitized:
            # collector-loss-stack: the supervisor's own stack runs out, which only a handler on
            # an alternate stack can still report (POSIX; Windows is a stated known limit).
            for mode in ("collector-loss","collector-loss-abort","fault-after-loss")+(("collector-loss-stack",) if os.name!="nt" else ()):
                p,worker=spawn(mode)
                try:
                    with watchdog(p,worker):
                        assert p.stdout.readline()==b"decision collector\n",mode
                        # the supervisor died of its own crash, not of a kill from outside
                        status=p.wait(timeout=30)
                        assert status!=0 and (os.name=="nt" or status in (-signal.SIGSEGV,-signal.SIGABRT)),(mode,status)
                        p.stdin.write(b"worker survived\n");p.stdin.flush();p.stdin.close();p.stdin=None
                        output,errors=read_until_eof(p,worker)
                        if mode=="fault-after-loss": assert output==b"",(mode,output)
                        else: assert output==b"worker survived\n",(mode,output)
                finally:
                    p.stdout.close();p.stderr.close()
    for name,mode,lines,markers in (
            ("supervisor-killed","supervisor-killed",b"decision launcher\nstop requested\nstopped\n",0),
            ("supervisor-killed-hung","supervisor-killed-hung",b"decision launcher\nstop requested\n",1),
            # The marker goes after the program's own static teardown, not before it: a crash
            # there leaves it (the probe's destructor counts the markers it still finds).
            ("supervisor-killed-teardown","supervisor-killed-teardown",b"decision launcher\nstop requested\nstopped\nteardown markers 1\n",0)):
        with check(name):
            # The supervisor ends with no notice, as a launcher's hard kill leaves it (SIGKILL;
            # TerminateProcess on Windows, which is what p.kill() does there): the worker takes
            # the stop path a SIGTERM takes and is gone. The hung variant has a worker whose
            # stop never finishes: it is ended within the bound (the fixture shortens it to
            # 300 ms), and the test passes by the worker being gone. A stop that finishes removes
            # the worker's own run marker, so that a later start does not read the run as an
            # unexpected end; a hard exit leaves it, as the end it is.
            p,worker=spawn(mode)
            try:
                with watchdog(p,worker):
                    p.kill();p.wait(timeout=30)
                    output,errors=read_until_eof(p,worker)
                    assert output==lines,(output,errors)
                    assert worker_gone(worker),worker
                    assert len(list((base/mode/"app"/"diagnostics").glob("*.run")))==markers,(name,"run markers")
                    # The dead supervisor's hold goes with a marker the worker removed, and stays
                    # with one it left (both free: the run reads as one that wrote no report).
                    assert len(list((base/mode/"app"/"diagnostics").glob("*.pub")))==markers,(name,"holds")
            finally:
                p.stdout.close();p.stderr.close()
    with check('watch-channel'):
        if os.name!="nt":
            # The watcher on a channel this test holds the other end of. End of file with
            # nothing before it is the launcher's stop. The notice, or any other byte, ends
            # the watching without a stop: it never consumes the byte and never spins on it.
            import socket
            for name,sent,expected in (
                    ("end of file",b"",b"decision launcher\nstop requested\nstopped\n"),
                    ("notice",b"F",b"decision collector\npending 46\ncontinued\n"),
                    ("unexpected byte",b"X",b"decision unknown\npending 58\ncontinued\n"),
                    ("acknowledgement",b"A",b"decision unknown\npending 41\ncontinued\n"),
                    ("garbage",b"\x00\xff"*64,b"decision unknown\npending 00\ncontinued\n")):
                parent,child=socket.socketpair()
                p=None
                try:
                    root=base/("watch-"+name.replace(" ","-"))/"app"/"diagnostics"
                    p=subprocess.Popen([binary,str(root),"watch-direct",str(child.fileno())],
                        pass_fds=(child.fileno(),),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                    child.close()
                    assert p.stdout.readline()==b"ready\n",name
                    int(p.stdout.readline())  # the process id the body prints first
                    if sent: parent.sendall(sent)
                    parent.close()  # end of file follows whatever was sent
                    try: out,err=p.communicate(b"continued\n",timeout=30)
                    except subprocess.TimeoutExpired:
                        p.kill();p.communicate();raise AssertionError(name+": hung")
                    assert out==expected,(name,out,err)
                    assert p.returncode==0,(name,p.returncode,err)
                finally:
                    parent.close();child.close()
                    if p and p.poll() is None: p.kill();p.wait()

    with check('withheld-ack'):
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

    def check_dropped(report,mode):
        # Everything left out is counted, by kind; a clean run leaves nothing out.
        dropped=report["dropped"]
        assert sorted(dropped)==sorted(["threads","unregistered_threads","plugins","commands","cells","messages","untracked","bridges"]),(mode,dropped)
        assert all(isinstance(v,int) and v>=0 for v in dropped.values()),(mode,dropped)
        assert dropped["plugins"]==0 and dropped["messages"]==0 and dropped["untracked"]==0,(mode,dropped)
        # No fixture here starts a bridge process: the section is there, and empty (topic 6a).
        assert report["bridges"]==[] and dropped["bridges"]==0,(mode,report["bridges"])

    with check('inventory'):
        # The plug-in inventory (ADR-0083, topic 5c). The fixture announces a retired instance
        # and a live one, with strings that must arrive bounded and clean, then faults while a
        # thread is inside an entry of the live one. What names the entry (the execution table)
        # needs the collector to read the process; what names the instance travels the
        # channel, so the list itself is checked on every platform and in every build.
        def inventory_run(mode):
            root=base/mode/"app"/"diagnostics"
            run=subprocess.run([binary,str(root),mode],capture_output=True,timeout=60)
            assert run.returncode!=0,(mode,run)
            found=list(root.glob("*.json"))
            assert len(found)==1,(mode,found,run.stderr)
            raw=found[0].read_text()
            return raw,json.loads(raw)
        for mode,state,entry in (("inventory-process","active","process"),("inventory-state","active","state"),
                                 ("inventory-constructor","loading","create"),("inventory-other","active","params")):
            raw,report=inventory_run(mode)
            plugins=report["plugins"]
            assert len(plugins)==2,(mode,plugins)
            retired,live=plugins
            assert retired["state"]=="destroyed" and retired["name"]=="Retired Canary",retired
            assert [p["id"] for p in plugins]==[0,1],plugins
            assert live["state"]==state,(mode,live)
            assert (live["kind"],live["origin"],live["mode"])==("clap","foreign","in_process"),live
            # Bounded (64 bytes) and clean: a control character became "?", the vendor was cut.
            assert live["name"]=="Canary Reverb?[31m" and live["vendor"]=="V"*64,live
            assert live["version"]=="1.0??" and live["plugin_id"]=="com.canary.reverb",live
            for forbidden in ("\\u001b","\x1b","\"image\"","\"cell\"","\"generation\""):
                assert forbidden not in raw,(mode,forbidden)
            assert not any("inventory messages" in u for u in report["unavailable"]),report["unavailable"]
            check_dropped(report,mode)
            assert report["attribution"]["confidence"] in ("unknown","fault_in_image","active_token"),report["attribution"]
            threads=report["threads"]
            tied=bool(threads) and threads[0].get("state")=="walked" and "entry" in threads[0]
            if sanitized or not tied: continue  # the collector read nothing it could tie
            assert threads[0]["role"]=="fault" and threads[0]["entry"]==entry,(mode,threads[0])
            executing=live.get("executing")
            assert executing and executing[0]=={"entry":entry,"thread":0,"match":"thread"},(mode,live)
            if mode=="inventory-other":
                assert len(executing)==2 and executing[1]["entry"]=="process" and executing[1]["match"]=="other_thread" and executing[1]["thread"]>0,executing
            else:
                assert len(executing)==1,executing
            assert "executing" not in retired,retired
            # What the report concludes: the fault is in the instance's own binary when the
            # module has an identity, and otherwise the table's token on the fault thread says it.
            fault_module=threads[0]["frames"][0].get("module")
            has_id=fault_module is not None and bool(report["modules"][fault_module]["id"])
            assert report["attribution"]=={"confidence":"fault_in_image" if has_id else "active_token","plugin":live["id"]},(mode,report["attribution"])
            # The binary the instance came from is the module the fault is in.
            module=threads[0]["frames"][0].get("module")
            if module is not None and report["modules"][module]["id"]:
                assert live.get("build")=={"format":report["modules"][module]["format"],"id":report["modules"][module]["id"]},(live,report["modules"][module])

    with check('commands'):
        # Command breadcrumbs (ADR-0083, topic 5d), through a real dispatcher: commands from
        # several origins run, an unknown name is refused, then one faults in its handler. The
        # ring lives in the process's execution table and the names come over the channel, so
        # the entries are checked wherever the collector (or the macOS capsule) delivers them.
        def commands_run(mode):
            root=base/mode/"app"/"diagnostics"
            run=subprocess.run([binary,str(root),mode],capture_output=True,timeout=60)
            assert run.returncode!=0,(mode,run)
            found=list(root.glob("*.json"))
            assert len(found)==1,(mode,found,run.stderr)
            raw=found[0].read_text()
            return raw,json.loads(raw)
        def named(report): return [(c["name"],c["origin"],c["state"]) for c in report["commands"]]
        before=[("fixture.fine","ui","ended"),("fixture.note-secret","script","ended")]
        for mode,after in (("commands-real",[("fixture.crash","agent","begun")]),
                           ("commands-batch",[("batch","rpc","begun"),("fixture.fine","rpc","ended"),("fixture.crash","rpc","begun")]),
                           ("commands-other",[])):
            raw,report=commands_run(mode)
            if not report["commands"]:
                # The collector read nothing (restricted tracing on Linux, a sanitizer's layout)
                # and the report must say so; anywhere it can read the process the ring is there.
                assert any("execution table" in u for u in report["unavailable"]),report["unavailable"]
                restricted=False
                if sys.platform.startswith("linux"):
                    try: restricted=int(pathlib.Path("/proc/sys/kernel/yama/ptrace_scope").read_text())>=2 and os.geteuid()!=0
                    except OSError: pass
                assert sanitized or restricted,(mode,"no commands where the table is readable",report["unavailable"])
                continue
            assert named(report)==before+after,(mode,named(report))
            check_dropped(report,mode)
            # No plug-in is involved: what the model had been doing is all there is to go on.
            assert report["attribution"]=={"confidence":"recent_command","plugin":None},(mode,report["attribution"])
            assert report["dropped"]["commands"]==(1 if mode=="commands-other" else 0),(mode,report["dropped"])
            assert [c["seq"] for c in report["commands"]]==sorted(c["seq"] for c in report["commands"])
            # Only names, categories and states reach the file: no argument, client, agent,
            # label, or name the registry never held.
            for forbidden in ("canary","no-such-command"):
                assert forbidden not in raw,(mode,forbidden)
            assert any("last 64 the model thread began" in u for u in report["unavailable"])
            refused=any("other than the model thread" in u for u in report["unavailable"])
            assert refused==(mode=="commands-other"),(mode,report["unavailable"])

    with check('linux traces'):
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
            # Four threads of the fixture and the supervisor watcher (ADR-0091), which is no
            # registered thread: an `other` thread parked in the channel wait.
            assert len(threads)==5,threads
            assert [t["role"] for t in threads].count("other")==1,threads
            # Roles come from the process's execution table (topic 5): main registered itself
            # as the model thread, the parked workers as numbered audio workers, and the
            # faulting thread keeps "fault" and says what it was.
            assert [t["role"] for t in threads[:2]]==["fault","model"],threads
            assert threads[0]["registered"]=="audio_render" and "worker" not in threads[0],threads[0]
            assert sorted((t["role"],t["worker"]) for t in threads[2:] if t["role"]!="other")==[("audio_background",1),("audio_live",2)],threads
            assert [t["ordinal"] for t in threads]==[0,1,2,3,4]
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
                    if t["role"]=="other": continue  # the supervisor watcher, parked in a wait
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
                    assert text.returncode==0 and b"faultOuter" in text.stdout and b"thread 0 fault (audio_render): walked" in text.stdout,text
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
                assert not list(root.glob("*.pub")),"supervisor hold left behind"
                report=json.loads(found[0].read_text())
                assert report["kind"]=="crash" and report["terminal_context"],report
                assert report["native_fatal_status"] and report["code"]==signal.SIGSEGV,report
                by_role={}
                for t in report["threads"]: by_role.setdefault(t["role"],[]).append(t)
                assert by_role["fault"][0]["state"]=="walked",report["threads"]
                assert [t.get("reason") for t in report["threads"] if t["state"]=="unavailable"]==["deadline"],report["threads"]
            # Execution scopes (topic 5b): each thread of the fixture is inside an entry of an
            # instance of its own when the fault comes, and the report names it. One thread is
            # unregistered (found by the stack it stands on) and one is in two nested scopes
            # (reported in the inner). No cell or address is in the report.
            expected,raw,report=trace_run("trace-scopes")
            assert report["kind"]=="crash" and report["terminal_context"],report
            threads=report["threads"]
            assert len(threads)==6,threads  # the five of the fixture and the supervisor watcher
            assert [t["role"] for t in threads[:2]]==["fault","model"],threads
            assert threads[0]["registered"]=="audio_render" and threads[0]["entry"]=="process",threads[0]
            restricted=yama_scope()>=2 and os.geteuid()!=0
            if not restricted:
                assert threads[1]["entry"]=="params",threads[1]
                entries={(t["role"],t.get("worker")):t.get("entry") for t in threads[2:] if "entry" in t}
                assert entries=={("audio_live",2):"reset",("audio_background",1):"library_open",("other",None):"gui"},threads
                assert "plug-in entries" not in raw and "no thread read could be tied" not in raw,raw
            for forbidden in ("\"cell\"","generation","\"stack\""):
                assert forbidden not in raw,forbidden
            if os.geteuid()!=0:
                # A non-dumpable worker refuses every external read: all threads unavailable.
                expected,raw,report=trace_run("trace-nondumpable")
                assert report["kind"]=="crash",report
                assert len(report["threads"])>=1
                for t in report["threads"]:
                    assert t["state"]=="unavailable" and t["reason"]=="maps_unreadable",t
                    assert t["frames"]==[],t

    with check('windows traces'):
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
            assert [t["role"] for t in threads[:2]]==["fault","model"],threads
            assert threads[0]["registered"]=="audio_render",threads[0]
            roles=[(t["role"],t.get("worker")) for t in threads[2:]]
            assert ("audio_live",2) in roles and ("audio_background",1) in roles,threads
            assert all(r in ("audio_live","audio_background","other") for r,_ in roles),threads
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
            # Execution scopes (topic 5b), as on Linux: the fault thread and the registered ones
            # by their slots' stack bounds, the unregistered one by its allocation.
            run=subprocess.run([binary,str(root),"trace-scopes",str(base/"scopes-expected.json")],capture_output=True,timeout=120)
            assert run.returncode!=0,run
            found=sorted(root.glob("*.json"),key=lambda p:p.stat().st_mtime)
            assert len(found)==2,(found,run.stderr)
            scoped=json.loads(found[-1].read_text())
            threads=scoped["threads"]
            assert threads[0]["registered"]=="audio_render" and threads[0]["entry"]=="process",threads[0]
            assert threads[1]["entry"]=="params",threads[1]
            entries={(t["role"],t.get("worker")):t.get("entry") for t in threads[2:] if "entry" in t}
            assert entries.get(("audio_live",2))=="reset" and entries.get(("audio_background",1))=="library_open",threads
            assert entries.get(("other",None))=="gui",threads

if failures:
    print("%d section(s) failed: %s"%(len(failures),", ".join(failures)),file=sys.stderr)
    sys.exit(1)
print("diagnostics lifecycle: normal/error/teardown/stdio/collector loss passed")
