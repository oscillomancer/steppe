# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
"""Headless CLI validation and the public MCP tool catalogue."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)


class NumericArguments(unittest.TestCase):
    def test_invalid_numbers(self):
        with tempfile.TemporaryDirectory(prefix="supple-cli-") as root:
            missing = str(Path(root) / "missing.json")
            for option, value in [
                ("bar", "1junk"), ("bar", "9" * 40), ("block", "128junk"),
                ("inputs", "0x"), ("outputs", "2.5"), ("rate", "48000oops"),
                ("seconds", "garbage"), ("seconds", "nan"), ("seconds", "inf"),
                ("seconds", "1e999"), ("seconds", "-1"), ("seconds", ""),
            ]:
                with self.subTest(option=option, value=value):
                    result = subprocess.run(
                        [BINARY, "play", missing, "--backend", "dummy", "--" + option, value],
                        capture_output=True, text=True, check=False,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    error = json.loads(result.stdout)
                    self.assertEqual(error["code"], "cli.number")
                    self.assertEqual(error["path"], "/" + option)
                    self.assertTrue(error["message"])
                    self.assertTrue(error["hint"])

    def test_valid_dummy_playback(self):
        with tempfile.TemporaryDirectory(prefix="supple-cli-") as root:
            project = str(Path(root) / "project.json")
            invocation = json.dumps({"command": "project.set", "args": {"name": "CLI test"}})
            subprocess.run(
                [BINARY, "exec", invocation, "--project", project, "--save", "--workdir", root],
                capture_output=True, text=True, check=True,
            )
            result = subprocess.run(
                [BINARY, "play", project, "--backend", "dummy", "--seconds", "0",
                 "--bar", "1", "--block", "128", "--rate", "4.8e4", "--inputs", "0",
                 "--outputs", "2"],
                capture_output=True, text=True, check=True,
            )
            self.assertEqual(json.loads(result.stdout)["backend"], "dummy")


class McpTools(unittest.TestCase):
    CORE = set("""
        project.get project.open project.save session.describe
        track.add track.set track.list track.remove
        item.add item.set item.remove midi.write midi.get
        fx.add fx.describe fx.patch.get fx.patch.merge fx.remove plugin.list
        route.add env.add env.points.set tempo.set
        transport.play transport.stop transport.seek
        render.mix analyze.track analyze.render analyze.compare
        branch.create branch.switch branch.diff branch.merge history.undo batch engine.start
    """.split())

    def exchange(self, *options):
        requests = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-06-18", "clientInfo": {"name": "cli-test"}}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        ]
        for name, arguments in [
            ("describe", {}),
            ("describe", {"command": "fx.patch.list"}),
            ("exec", {"command": "fx.patch.list", "args": {"fx": "fx_999"}}),
            ("query", {"command": "fx.patch.list", "args": {"fx": "fx_999"}}),
            ("describe", {"text": "fx.patch"}),
            ("exec", {"command": "engine.status"}),
            ("query", {"command": "engine.status"}),
        ]:
            requests.append({"jsonrpc": "2.0", "id": len(requests) + 1, "method": "tools/call",
                             "params": {"name": name, "arguments": arguments}})
        with tempfile.TemporaryDirectory(prefix="supple-mcp-tools-") as root:
            result = subprocess.run([BINARY, "--mcp", "--workdir", root, *options],
                                    input="".join(json.dumps(r) + "\n" for r in requests),
                                    capture_output=True, text=True, check=True)
        return result.stdout.splitlines()

    def test_core_and_all(self):
        registry = json.loads(subprocess.run([BINARY, "registry"], capture_output=True,
                                             text=True, check=True).stdout)["commands"]
        public = {c["name"].replace(".", "_") for c in registry.values()
                  if c["exposure"] != "internal"}
        expected = {name.replace(".", "_") for name in self.CORE} | {"describe", "exec", "query"}
        for options in [(), ("--tools", "core"), ("--tools", "all")]:
            with self.subTest(options=options):
                lines = self.exchange(*options)
                replies = [json.loads(line)["result"] for line in lines]
                tools = replies[1]["tools"]
                names = {t["name"] for t in tools}
                self.assertEqual(len(names), len(tools))
                self.assertEqual(names, public | {"describe", "exec", "query"}
                                 if options == ("--tools", "all") else expected)
                if options != ("--tools", "all"):
                    self.assertLessEqual(len(lines[1].encode("utf-8")) + 1, 60000)
                for tool in tools:
                    if tool["name"] not in {"describe", "exec", "query"}:
                        self.assertNotIn("outputSchema", tool)
                index = replies[2]["structuredContent"]
                self.assertEqual(index["columns"], ["name", "kind", "summary"])
                rows = [row for group in index["namespaces"].values() for row in group]
                self.assertEqual({r[0] for r in rows}, {c["name"] for c in registry.values()})
                self.assertLess(len(json.dumps(index, separators=(",", ":")).encode()), 25000)
                for name, kind, summary in rows:
                    self.assertIn(kind, {"query", "edit", "action"})
                    self.assertLessEqual(len(summary), 80)
                self.assertIn("fx.patch.list", {r[0] for r in rows})
                detail = replies[3]["structuredContent"]
                for field in ("args", "result", "errors", "examples"):
                    self.assertIn(field, detail)
                # No plugin is needed: the real handler must reject the missing FX.
                for response in replies[4:6]:
                    self.assertTrue(response["isError"])
                    self.assertEqual(response["structuredContent"]["code"], "fx.unknown")
                filtered = replies[6]["structuredContent"]["namespaces"]
                self.assertEqual(set(filtered), {"fx"})
                self.assertTrue(all("fx.patch" in r[0] for r in filtered["fx"]))
                for response in replies[7:9]:
                    self.assertFalse(response.get("isError"))
                    self.assertFalse(response["structuredContent"]["running"])

    def test_invalid_tools_option(self):
        for options in [("--tools",), ("--tools", "other")]:
            result = subprocess.run([BINARY, "--mcp", *options], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("--tools core|all", result.stderr)


if __name__ == "__main__":
    unittest.main()
