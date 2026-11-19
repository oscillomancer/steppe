#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise engine/sandbox socket servers through the stdio MCP proxy."""
import json
import os
from pipe_ready import wait_pipe
import processes
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    server_binary, proxy_binary, target = sys.argv[1:4]
    engine_mode = target == "--engine"
    started = time.monotonic()
    temporary = tempfile.mkdtemp(prefix="supple-proxy-test-")
    try:
        socket = Path(temporary) / "control.sock"
        command = [server_binary, "serve", "--socket", str(socket), "--workdir", temporary]
        if not engine_mode:
            command += ["--plugin-path", str(Path(target).parent)]
        server = subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        proxy = None
        try:
            for _ in range(1200):  # up to 30 s: sanitizer builds start slowly
                if socket.exists():
                    break
                if server.poll() is not None:
                    raise AssertionError(server.stderr.read().decode())
                time.sleep(0.025)
            assert socket.exists(), "socket server did not start"
            proxy = subprocess.Popen(
                [proxy_binary, "--socket", str(socket)], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
            )
            pending = bytearray()

            def call(identifier, method, params):
                message = {"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}
                os.write(proxy.stdin.fileno(), (json.dumps(message) + "\n").encode())
                while b"\n" not in pending:
                    readable = wait_pipe(proxy.stdout, 30)
                    assert readable, f"proxy timed out on {method}"
                    data = os.read(proxy.stdout.fileno(), 8192)
                    assert data, f"proxy exited on {method}: {proxy.stderr.read().decode()}"
                    pending.extend(data)
                index = pending.index(b"\n")
                result = json.loads(pending[:index])
                del pending[:index + 1]
                assert result["id"] == identifier, result
                return result

            init = call(1, "initialize", {"protocolVersion": "2025-06-18",
                                         "clientInfo": {"name": "proxy-test"}})
            assert init["result"]["protocolVersion"] == "2025-06-18", init
            listed = call(2, "tools/list", {})
            names = {tool["name"] for tool in listed["result"]["tools"]}
            if engine_mode:
                assert "session_describe" in names, names
                described = call(3, "tools/call", {"name": "session_describe", "arguments": {}})
                assert not described["result"].get("isError"), described
            else:
                assert {"plugin_load", "render"} <= names, names
                loaded = call(3, "tools/call", {"name": "plugin_load",
                                                "arguments": {"plugin": target}})
                assert not loaded["result"].get("isError"), loaded
                rendered = call(4, "tools/call", {"name": "render", "arguments": {
                    "notation": "voice lead; 1: C4:q r:h r:q", "tail_s": 0.0}})
                assert not rendered["result"].get("isError"), rendered
                assert Path(rendered["result"]["structuredContent"]["path"]).exists(), rendered
            proxy.stdin.close()
            assert proxy.wait(timeout=10) == 0, proxy.stderr.read().decode()

            # The CLI must forward --tools all to the proxy catalogue builder.
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                    "protocolVersion": "2025-06-18", "clientInfo": {"name": "all-tools-test"}}},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            ]
            all_tools = subprocess.run(
                [proxy_binary, "--socket", str(socket), "--tools", "all"],
                input="".join(json.dumps(m) + "\n" for m in messages),
                capture_output=True, text=True, check=True,
            )
            all_listed = json.loads(all_tools.stdout.splitlines()[1])["result"]["tools"]
            registry = json.loads(subprocess.run(
                [server_binary, "registry"], capture_output=True, text=True, check=True,
            ).stdout)["commands"]
            generic = {"describe", "exec"} | ({"query"} if engine_mode else set())
            public = {c["name"].replace(".", "_") for c in registry.values()
                      if c["exposure"] != "internal"}
            assert {t["name"] for t in all_listed} == public | generic
            assert names <= {t["name"] for t in all_listed}
            if engine_mode:
                assert "fx_patch_list" not in names
                assert "fx_patch_list" in {t["name"] for t in all_listed}
            for options in [("--tools",), ("--tools", "invalid")]:
                invalid = subprocess.run([proxy_binary, *options], capture_output=True, text=True)
                assert invalid.returncode == 2, invalid
                assert "--tools core|all" in invalid.stderr
        finally:
            if proxy and proxy.poll() is None:
                proxy.kill()
                proxy.wait(timeout=5)
            # Ends the server and, on Windows, the program its supervisor started.
            processes.stop(server, timeout=5)
            # POSIX terminate reaches the supervisor's handler, which passes it on to the
            # program, so the socket is gone. On Windows the program's lock file can stay
            # open for a moment after taskkill returns; remove_tree below waits for that.
            if os.name != "nt":
                assert not socket.exists(), "server left its socket after shutdown"
    finally:
        processes.remove_tree(temporary)
    print(f"MCP proxy socket integration passed in {time.monotonic() - started:.2f}s")


if __name__ == "__main__":
    main()
