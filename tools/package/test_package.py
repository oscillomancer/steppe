# SPDX-License-Identifier: GPL-3.0-or-later
"""Installer ownership/collision checks and strict dependency audit regressions."""
import os
import plistlib
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import linux
import windows
import macos
from common import ROOT


@unittest.skipUnless(os.name == "posix" and os.uname().sysname == "Linux", "Linux packaging")
class Installer(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="install-test-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "download"
        self.prefix = self.root / 'prefix with spaces & dollars$'
        self.home = self.root / "home"
        self.home.mkdir()
        self.env = dict(os.environ, HOME=str(self.home))
        # Control cache generation instead of depending on desktop-file-utils' version.
        tools = self.root / "tools"
        tools.mkdir()
        desktop_database = tools / "update-desktop-database"
        desktop_database.write_text(
            '#!/bin/sh\n'
            'printf "[MIME Cache]\\n" > "$1/mimeinfo.cache"\n'
            'if [ "$PACKAGE_TEST_INTENT_CACHE" = 1 ]; then\n'
            '  printf "[Intent Cache]\\n" > "$1/intent.cache"\n'
            'fi\n')
        desktop_database.chmod(0o755)
        self.env.update(PATH=str(tools) + os.pathsep + os.environ.get("PATH", os.defpath),
                        PACKAGE_TEST_INTENT_CACHE="0")
        for rel in ("bin", "lib/example/plugins", "share/applications", "share/icons/hicolor/16x16/apps"):
            (self.source / rel).mkdir(parents=True)
        for name in ("example", "example-engine", "example-mcp", "example-bridge", "example-scanner"):
            (self.source / "bin" / name).write_text("program")
        (self.source / "lib/example/plugins/Stock.clap").write_text("plugin")
        (self.source / "lib/example/plugins/Stock-LICENSE.txt").write_text("notice")
        (self.source / "share/applications/example.desktop").write_text("[Desktop Entry]\nExec=example %f\n")
        (self.source / "share/icons/hicolor/16x16/apps/example.png").write_text("icon")
        for script in ("install", "uninstall"):
            text = (ROOT / f"tools/package/{script}.sh.in").read_text()
            target = self.source / f"{script}.sh"
            target.write_text(text.replace("@PRODUCT_SLUG@", "example").replace("@PRODUCT_NAME@", "Example"))
            target.chmod(0o755)

    def install(self, *extra):
        return subprocess.run([str(self.source / "install.sh"), "--prefix", str(self.prefix), *extra],
                              env=self.env, capture_output=True, text=True)

    def test_install_and_remove_only_owned_paths(self):
        other = self.home / ".clap/Other.clap"
        other.parent.mkdir()
        other.write_text("unrelated")
        for intent_cache in (False, True):
            with self.subTest(intent_cache=intent_cache):
                self.env["PACKAGE_TEST_INTENT_CACHE"] = "1" if intent_cache else "0"
                result = self.install("--plugins")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((self.prefix / "bin/example-mcp").is_symlink())
                desktop = (self.prefix / "share/applications/example.desktop").read_text()
                self.assertIn(r'prefix with spaces & dollars\\$', desktop)
                self.assertTrue((self.home / ".clap/Stock.clap").is_file())
                cache = self.prefix / "share/applications/intent.cache"
                self.assertEqual(cache.is_file(), intent_cache)
                manifest = (self.prefix / "opt/example/.installed-files").read_text().splitlines()
                self.assertEqual(str(cache) in manifest, intent_cache)
                result = subprocess.run([str(self.prefix / "opt/example/uninstall.sh"), "--prefix", str(self.prefix)],
                                        env=self.env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(cache.exists())
                self.assertFalse(self.prefix.exists())
                self.assertEqual(other.read_text(), "unrelated")
                self.assertFalse((self.home / ".clap/Stock.clap").exists())
                self.assertTrue(self.root.exists(), "Uninstall must not prune above prefix")

    def test_collision_writes_nothing(self):
        conflict = self.home / ".clap/Stock.clap"
        conflict.parent.mkdir()
        conflict.write_text("existing")
        self.assertNotEqual(self.install("--plugins").returncode, 0)
        self.assertEqual(conflict.read_text(), "existing")
        self.assertFalse(self.prefix.exists())

    def test_plugins_are_opt_in(self):
        self.assertEqual(self.install().returncode, 0)
        self.assertFalse((self.home / ".clap").exists())

    def test_upgrade_replaces_owned_files_and_preserves_user_data(self):
        self.assertEqual(self.install("--plugins").returncode, 0)
        private_note = self.prefix / "opt/example/my-project.json"
        private_note.write_text("user project")
        (self.source / "bin/example").write_text("new version")
        (self.source / "lib/example/plugins/Stock.clap").write_text("new plugin")
        result = self.install("--plugins")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.prefix / "bin/example").read_text(), "new version")
        self.assertEqual((self.home / ".clap/Stock.clap").read_text(), "new plugin")
        self.assertEqual(private_note.read_text(), "user project")
        self.assertEqual(subprocess.run([str(self.prefix / "opt/example/uninstall.sh"), "--prefix", str(self.prefix)],
                                       env=self.env, capture_output=True).returncode, 0)
        self.assertEqual(private_note.read_text(), "user project")
        self.assertFalse((self.prefix / "bin/example").exists())

    def test_installed_installer_does_not_delete_itself(self):
        self.assertEqual(self.install().returncode, 0)
        result = subprocess.run([str(self.prefix / "opt/example/install.sh"), "--prefix", str(self.prefix) + "/."],
                                env=self.env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.prefix / "bin/example").is_file())

    def test_upgrade_remembers_external_plugins(self):
        self.assertEqual(self.install("--plugins").returncode, 0)
        (self.source / "lib/example/plugins/Stock.clap").write_text("new plugin")
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.home / ".clap/Stock.clap").read_text(), "new plugin")

    def test_upgrade_collision_preserves_previous_installation(self):
        self.assertEqual(self.install("--plugins").returncode, 0)
        private_note = self.prefix / "opt/example/my-project.json"
        private_note.write_text("user project")
        (self.source / "my-project.json").write_text("new shipped file")
        (self.source / "bin/example").write_text("new program")
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Already exists:", result.stderr)
        self.assertEqual((self.prefix / "bin/example").read_text(), "program")
        self.assertEqual((self.home / ".clap/Stock.clap").read_text(), "plugin")
        self.assertEqual(private_note.read_text(), "user project")
        self.assertTrue((self.prefix / "opt/example/uninstall.sh").is_file())

    def test_upgrade_foreign_ancestor_preserves_previous_installation(self):
        self.assertEqual(self.install().returncode, 0)
        (self.prefix / "opt/example/new-folder").write_text("user file")
        (self.source / "new-folder").mkdir()
        (self.source / "new-folder/new-file").write_text("new shipped file")
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.prefix / "bin/example").read_text(), "program")
        self.assertEqual((self.prefix / "opt/example/new-folder").read_text(), "user file")

    def test_uninstall_preserves_foreign_empty_icon_directories(self):
        unrelated = self.prefix / "share/icons/other-theme/32x32/apps"
        unrelated.mkdir(parents=True)
        self.assertEqual(self.install().returncode, 0)
        result = subprocess.run([str(self.prefix / "opt/example/uninstall.sh"), "--prefix", str(self.prefix)],
                                env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertTrue(unrelated.is_dir())
        self.assertFalse((self.prefix / "share/icons/hicolor").exists())

    def test_uninstall_refuses_replaced_payload_directory(self):
        self.assertEqual(self.install().returncode, 0)
        app = self.prefix / "opt/example"
        outside = self.root / "foreign-host-data"
        outside.mkdir()
        original = app / "lib/example/plugins"
        original.rename(original.with_name("plugins-original"))
        original.symlink_to(outside, target_is_directory=True)
        foreign = outside / "Stock-LICENSE.txt"
        foreign.write_text("another host owns this notice")
        result = subprocess.run([str(app / "uninstall.sh"), "--prefix", str(self.prefix)],
                                env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(foreign.read_text(), "another host owns this notice")
        self.assertTrue((app / ".installed-files").is_file())
        self.assertTrue((app / "bin/example").is_file(), "Refusal must happen before any removal")

    def test_uninstall_refuses_linked_app_root(self):
        self.assertEqual(self.install().returncode, 0)
        app = self.prefix / "opt/example"
        moved = self.root / "moved-app"
        app.rename(moved)
        app.symlink_to(moved, target_is_directory=True)
        result = subprocess.run([str(app / "uninstall.sh"), "--prefix", str(self.prefix)],
                                env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((moved / ".installed-files").is_file())

    def test_uninstall_keeps_path_based_external_entries(self):
        self.assertEqual(self.install("--plugins").returncode, 0)
        for original in (self.prefix / "bin", self.home / ".clap"):
            moved = self.root / (original.name + "-elsewhere")
            original.rename(moved)
            original.symlink_to(moved, target_is_directory=True)
        result = subprocess.run([str(self.prefix / "opt/example/uninstall.sh"), "--prefix", str(self.prefix)],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / "bin-elsewhere/example").exists())
        self.assertFalse((self.root / ".clap-elsewhere/Stock.clap").exists())

    def test_uninstall_validates_all_manifest_paths_before_removal(self):
        self.assertEqual(self.install().returncode, 0)
        app = self.prefix / "opt/example"
        foreign = self.prefix / "private-project.json"
        foreign.write_text("user project")
        manifest = app / ".installed-files"
        directories = app / ".installed-dirs"
        original_files = manifest.read_text()
        original_dirs = directories.read_text()
        for target, entry in ((manifest, str(app / "../../private-project.json")),
                              (manifest, str(foreign)),
                              (directories, str(app / "../../empty-project"))):
            with self.subTest(entry=entry):
                target.write_text(target.read_text() + entry + "\n")
                result = subprocess.run([str(app / "uninstall.sh"), "--prefix", str(self.prefix)],
                                        env=self.env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue((app / "bin/example").is_file(), "Refusal must precede any removal")
                self.assertEqual(foreign.read_text(), "user project")
                self.assertTrue(manifest.is_file())
                self.assertTrue(directories.is_file())
                manifest.write_text(original_files)
                directories.write_text(original_dirs)


class Dependencies(unittest.TestCase):
    def check(self, output, symbols="", filename="app"):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "lib/example").mkdir(parents=True)
            (root / filename).write_bytes(b"\x7fELF")
            with patch("linux.subprocess.check_output", side_effect=lambda command, **kwargs: symbols if command[0] == "nm" else output):
                linux.check_dependencies(root, "example")

    def test_base_libraries(self):
        self.check("libc.so.6 => /lib/libc.so.6 (0x1)\n/lib64/ld-linux-x86-64.so.2 (0x2)\n"
                   "libbsd.so.0 => /lib/libbsd.so.0 (0x3)\nlibmd.so.0 => /lib/libmd.so.0 (0x4)\n")

    def test_undeclared_library(self):
        with self.assertRaisesRegex(RuntimeError, "unbundled dependency"):
            self.check("libsurprise.so.1 => /usr/lib/libsurprise.so.1 (0x1)\n")

    def test_missing_library(self):
        with self.assertRaisesRegex(RuntimeError, "unresolved"):
            self.check("libmissing.so.1 => not found\n")

    def test_jack_is_never_linked(self):
        with self.assertRaisesRegex(RuntimeError, "JACK"):
            self.check("libjack.so.0 => /usr/lib/libjack.so.0 (0x1)\n")
        with self.assertRaisesRegex(RuntimeError, "JACK"):
            self.check("", filename="libjack.so.0")

    def test_shared_cxx_runtimes_are_never_needed(self):
        for soname in ("libstdc++.so.6", "libgcc_s.so.1"):
            with self.subTest(soname=soname), self.assertRaisesRegex(RuntimeError, r"shared C\+\+ runtime"):
                self.check(f"{soname} => /usr/lib/{soname} (0x1)\n")

    def test_runtime_symbols_cannot_interpose(self):
        with self.assertRaisesRegex(RuntimeError, "runtime symbols are exported"):
            self.check("", "__cxa_throw T 123 45\n")

    def test_stock_entry_point_is_visible(self):
        with self.assertRaisesRegex(RuntimeError, "entry point"):
            self.check("", filename="Stock.clap")
        self.check("", "clap_entry D 123 45\n", filename="Stock.clap")


class MacSigning(unittest.TestCase):
    def test_bundle_rejects_dangling_or_external_runtime_links(self):
        with tempfile.TemporaryDirectory() as temp:
            app = Path(temp) / "Example.app"
            frameworks = app / "Contents/Frameworks"
            frameworks.mkdir(parents=True)
            outside = Path(temp) / "versioned.dylib"
            link = frameworks / "runtime.dylib"
            link.symlink_to(outside)
            for present in (False, True):
                if present:
                    outside.write_bytes(b"library")
                with self.assertRaisesRegex(RuntimeError, "symlink"):
                    macos.finish_bundle(app)

    def test_application_seal_follows_nested_bundle_signatures(self):
        with tempfile.TemporaryDirectory() as temp:
            app = Path(temp) / "Example.app"
            main = app / "Contents/MacOS/example"
            helper = app / "Contents/MacOS/example-mcp"
            plugin = app / "Contents/PlugIns/CLAP/Stock.clap"
            plugin_binary = plugin / "Contents/MacOS/Stock"
            for binary in (main, helper, plugin_binary):
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"code")
            (app / "Contents/Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable": "example"}))
            with patch("macos.is_macho", side_effect=lambda p: p in (main, helper, plugin_binary)), \
                 patch("macos.subprocess.check_output", return_value="binary:\n"), patch("macos.run") as run:
                macos.finish_bundle(app)
            signatures = [call.args[-1] for call in run.call_args_list if "--sign" in call.args]
            self.assertNotIn(main, signatures, "Signing main early seals the containing app")
            self.assertEqual(signatures[-2:], [plugin, app])
            self.assertLess(signatures.index(plugin_binary), signatures.index(plugin))
            self.assertIn(helper, signatures)


class WindowsManifest(unittest.TestCase):
    def test_runtime_audit_rejects_unrelated_staged_dll(self):
        with tempfile.TemporaryDirectory() as temp:
            stage = Path(temp)
            (stage / "SDL3.dll").write_bytes(b"runtime")
            (stage / "zlib1.dll").write_bytes(b"runtime")
            windows.check_runtime_dlls(stage, ["sdl3.dll", "zlib1.dll"])
            (stage / "opengl32.dll").write_bytes(b"unrelated CI renderer")
            with self.assertRaisesRegex(RuntimeError, "unexpected.*opengl32"):
                windows.check_runtime_dlls(stage, ["sdl3.dll", "zlib1.dll"])

    def test_uninstall_lists_only_staged_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            tree = root / "stage"
            (tree / "plugins/clap").mkdir(parents=True)
            (tree / "plugins/clap/Stock.clap").write_bytes(b"plugin")
            (tree / "app.exe").write_bytes(b"program")
            (root / "unrelated.txt").write_text("keep")
            windows.write_manifests(tree, root)
            manifest = (root / "app-files.nsh").read_text()
            self.assertIn('Delete "${root}\\app.exe"', manifest)
            self.assertIn('RMDir "${root}\\plugins\\clap"', manifest)
            self.assertNotIn("unrelated", manifest)
            self.assertNotIn("RMDir /r", manifest)
            self.assertNotIn('Delete "${root}\\*"', manifest)
            ownership = (tree / ".installed-files").read_text(encoding="utf-16-le")
            self.assertIn("F|app.exe\n", ownership)
            self.assertIn("D|plugins\\clap\n", ownership)
            self.assertIn("F|.installed-files\n", ownership)
            self.assertIn("F|uninstall.exe\n", ownership)
            optional = (tree / ".installed-plugins").read_text(encoding="utf-16-le")
            self.assertEqual(optional, "F|Stock.clap\n")
            self.assertNotIn("unrelated", ownership)
            self.assertLess(manifest.index('Call un.CheckDirectoryChain'), manifest.index('Delete "${root}\\app.exe"'))


if __name__ == "__main__":
    unittest.main()
