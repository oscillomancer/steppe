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
        route.add env.add env.points.set tempo.set modulate
        transport.play transport.stop transport.seek
        render.mix analyze.track analyze.render analyze.compare
        branch.create branch.switch branch.diff branch.merge history.undo batch engine.start
    """.split())

    # What each level of describe may cost an agent, in compact JSON bytes (the real registry; a
    # command or group that breaks one fails with its name). If a budget is reached, split the group
    # or shorten summaries rather than raising it.
    GROUPS_BUDGET = 5000       # describe {}: every group, one line each
    GROUP_BUDGET = 4000        # describe {group}: one group's commands
    COMMAND_BUDGET = 16000     # describe {command}: one command's whole contract
    SEARCH_BUDGET = 6000       # describe {text|kind|tag}: at most 60 rows
    MAX_GROUPS = 60

    def exchange(self, extra, *options):
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
        ] + extra:
            requests.append({"jsonrpc": "2.0", "id": len(requests) + 1, "method": "tools/call",
                             "params": {"name": name, "arguments": arguments}})
        with tempfile.TemporaryDirectory(prefix="supple-mcp-tools-") as root:
            result = subprocess.run([BINARY, "--mcp", "--workdir", root, *options],
                                    input="".join(json.dumps(r) + "\n" for r in requests),
                                    capture_output=True, text=True, check=True)
        return result.stdout.splitlines()

    @staticmethod
    def compact(value):
        return len(json.dumps(value, separators=(",", ":")).encode())

    def test_core_and_all(self):
        document = json.loads(subprocess.run([BINARY, "registry"], capture_output=True,
                                             text=True, check=True).stdout)
        registry = document["commands"]
        public = {c["name"].replace(".", "_") for c in registry.values()
                  if c["exposure"] != "internal"}
        public_names = sorted({c["name"] for c in registry.values()
                               if c["exposure"] != "internal"})
        by_group = {}
        for name in public_names:
            by_group.setdefault(name.split(".")[0], []).append(name)
        groups = sorted(by_group)
        expected = {name.replace(".", "_") for name in self.CORE} | {"describe", "exec", "query"}
        extra = [("describe", {"group": g}) for g in groups]
        extra += [("describe", {"command": n}) for n in public_names]
        extra += [("describe", {"group": "nonesuch"}), ("describe", {"text": "e"}),
                  ("describe", {"text": "automate"}), ("describe", {"text": "AUTOMATE"})]
        for options in [(), ("--tools", "core"), ("--tools", "all")]:
            with self.subTest(options=options):
                lines = self.exchange(extra, *options)
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
                # Level 0: the groups, one line each.
                index = replies[2]["structuredContent"]
                self.assertEqual(index["columns"], ["group", "count", "summary"])
                self.assertNotIn("namespaces", index)
                self.assertLessEqual(len(index["groups"]), self.MAX_GROUPS)
                self.assertLess(self.compact(index), self.GROUPS_BUDGET,
                                "describe {} is over its budget: shorten group lines or merge groups")
                self.assertEqual([row[0] for row in index["groups"]], groups)
                self.assertEqual(index["groups"] and {row[0]: row[2] for row in index["groups"]},
                                 {g: document["groups"][g] for g in groups})
                for group, count, summary in index["groups"]:
                    self.assertEqual(count, len(by_group[group]), group)
                    self.assertTrue(summary, group)
                    self.assertLessEqual(len(summary), 70, group)
                detail = replies[3]["structuredContent"]
                for field in ("args", "result", "errors", "examples"):
                    self.assertIn(field, detail)
                # No plugin is needed: the real handler must reject the missing FX.
                for response in replies[4:6]:
                    self.assertTrue(response["isError"])
                    self.assertEqual(response["structuredContent"]["code"], "fx.unknown")
                filtered = replies[6]["structuredContent"]
                self.assertEqual(filtered["columns"], ["name", "kind", "summary"])
                self.assertNotIn("truncated", filtered)
                self.assertEqual(set(filtered["namespaces"]), {"fx"})
                self.assertTrue(all("fx.patch" in r[0] for r in filtered["namespaces"]["fx"]))
                self.assertLess(self.compact(filtered), self.SEARCH_BUDGET)
                for response in replies[7:9]:
                    self.assertFalse(response.get("isError"))
                    self.assertFalse(response["structuredContent"]["running"])
                # Level 1: each group's commands, every public command reachable.
                rows = []
                for offset, group in enumerate(groups):
                    answer = replies[9 + offset]["structuredContent"]
                    self.assertEqual(answer["columns"], ["name", "kind", "summary"], group)
                    self.assertEqual(set(answer["namespaces"]), {group})
                    self.assertNotIn("truncated", answer, group)
                    self.assertLess(self.compact(answer), self.GROUP_BUDGET,
                                    f"group {group} is over its budget: split it or shorten summaries")
                    self.assertEqual([r[0] for r in answer["namespaces"][group]], by_group[group])
                    for name, kind, summary in answer["namespaces"][group]:
                        self.assertIn(kind, {"query", "edit", "action"})
                        self.assertLessEqual(len(summary), 80, name)
                    rows += answer["namespaces"][group]
                self.assertEqual({r[0] for r in rows}, set(public_names))
                self.assertIn("fx.patch.list", {r[0] for r in rows})
                # Level 2: one command's whole contract, under its budget.
                base = 9 + len(groups)
                for offset, name in enumerate(public_names):
                    answer = replies[base + offset]["structuredContent"]
                    self.assertEqual(answer["name"], name)
                    self.assertLess(self.compact(answer), self.COMMAND_BUDGET,
                                    f"describe {name} is over its budget: shorten its schemas or docs")
                base += len(public_names)
                unknown = replies[base]
                self.assertTrue(unknown["isError"])
                self.assertEqual(unknown["structuredContent"]["code"], "command.unknown_group")
                self.assertIn("fx", unknown["structuredContent"]["hint"])
                # A search that matches more than the cap says so and names the way out.
                capped = replies[base + 1]["structuredContent"]
                self.assertTrue(capped["truncated"])
                self.assertIn("narrow", capped["hint"])
                self.assertEqual(sum(len(g) for g in capped["namespaces"].values()), 60)
                self.assertLess(self.compact(capped), self.SEARCH_BUDGET)
                # Search ignores the case of ASCII letters: summaries start with a capital.
                for reply in (replies[base + 2], replies[base + 3]):
                    found = reply["structuredContent"]["namespaces"]
                    self.assertIn("env.add", {r[0] for rows in found.values() for r in rows})

    def test_registry_groups_on_the_command_line(self):
        groups = json.loads(subprocess.run([BINARY, "registry", "--groups"], capture_output=True,
                                           text=True, check=True).stdout)
        self.assertEqual(groups["columns"], ["group", "count", "summary"])
        group = json.loads(subprocess.run([BINARY, "registry", "--group", "fx"],
                                          capture_output=True, text=True, check=True).stdout)
        self.assertEqual(set(group["namespaces"]), {"fx"})
        wrong = subprocess.run([BINARY, "registry", "--group", "nonesuch"],
                               capture_output=True, text=True, check=False)
        self.assertNotEqual(wrong.returncode, 0)
        self.assertEqual(json.loads(wrong.stdout)["code"], "command.unknown_group")

    def test_invalid_tools_option(self):
        for options in [("--tools",), ("--tools", "other")]:
            result = subprocess.run([BINARY, "--mcp", *options], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("--tools core|all", result.stderr)


@unittest.skipIf(sys.platform == "win32", "a private folder needs the app's own ACLs on Windows")
class LastRunNotice(unittest.TestCase):
    """A service says, on stderr only, that the last run ended unexpectedly (topic 7b, P10)."""
    INCIDENT = "0" * 31 + "1"

    def folder(self, root):
        diagnostics = Path(root) / "diagnostics"
        diagnostics.mkdir(mode=0o700)
        # A run marker nobody holds and no report beside it: a run that ended with none written.
        marker = diagnostics / (self.INCIDENT + ".run")
        marker.write_text('{"role":"engine","schema":"supple.crash-run/1.0"}\n')
        marker.chmod(0o600)

    def serve(self, root):
        return subprocess.run([BINARY, "--mcp", "--workdir", root], input="", capture_output=True,
                              text=True, check=True)

    def test_service_says_so_once_acknowledged_no_more(self):
        with tempfile.TemporaryDirectory(prefix="supple-notice-") as root:
            self.folder(root)
            first = self.serve(root)
            self.assertIn("closed unexpectedly last time", first.stderr)
            self.assertIn("no crash report was written", first.stderr)
            self.assertIn("crash.acknowledge with an incident id", first.stderr)
            self.assertEqual(first.stdout, "")  # the protocol stream carries nothing of it
            # A one-shot call does not repeat it, and can read and dismiss it.
            listed = subprocess.run(
                [BINARY, "exec", json.dumps({"command": "crash.list", "args": {}}),
                 "--workdir", root], capture_output=True, text=True, check=True)
            self.assertEqual(listed.stderr, "")
            self.assertEqual(json.loads(listed.stdout)["unreported"][0]["incident"], self.INCIDENT)
            subprocess.run(
                [BINARY, "exec", json.dumps({"command": "crash.acknowledge",
                                             "args": {"incident": self.INCIDENT}}),
                 "--workdir", root], capture_output=True, text=True, check=True)
            self.assertEqual(self.serve(root).stderr, "")
            self.assertTrue((Path(root) / "diagnostics" / (self.INCIDENT + ".run")).exists())

    def test_the_apps_own_incident_is_not_the_services_to_announce(self):
        with tempfile.TemporaryDirectory(prefix="supple-notice-") as root:
            diagnostics = Path(root) / "diagnostics"
            diagnostics.mkdir(mode=0o700)
            marker = diagnostics / ("0" * 31 + "2.run")
            marker.write_text('{"role":"gui","schema":"supple.crash-run/1.0"}\n')
            marker.chmod(0o600)
            self.assertEqual(self.serve(root).stderr, "")  # the GUI's banner announces it
            # An explicit acknowledgement by name still works for any role.
            subprocess.run(
                [BINARY, "exec", json.dumps({"command": "crash.acknowledge",
                                             "args": {"incident": "0" * 31 + "2"}}),
                 "--workdir", root], capture_output=True, text=True, check=True)
            self.assertTrue((diagnostics / ("0" * 31 + "2.ack")).exists())

    def test_the_service_says_which_app_closed(self):
        with tempfile.TemporaryDirectory(prefix="supple-notice-") as root:
            self.folder(root)
            self.assertIn("engine closed unexpectedly last time", self.serve(root).stderr)

    def test_nothing_left_behind_says_nothing(self):
        with tempfile.TemporaryDirectory(prefix="supple-notice-") as root:
            self.assertEqual(self.serve(root).stderr, "")


if __name__ == "__main__":
    unittest.main()
