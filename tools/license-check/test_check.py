# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for first-party license-gate coverage."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path, PureWindowsPath
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("license_gate", Path(__file__).with_name("check.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class BuildFiles(unittest.TestCase):
    def check_fixture(self, filename, contents):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / filename
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(contents)
            (root / "THIRD_PARTY.md").write_text("Test fixture: no dependencies.\n")
            manifest = json.dumps({"allowed_licenses": ["MIT"], "dependencies": []})
            original_read = Path.read_text

            def read(path, *args, **kwargs):
                if path == root / "third_party/manifest.json":
                    return manifest
                return original_read(path, *args, **kwargs)

            # The fixture never creates or changes a third_party directory.
            with mock.patch.object(Path, "read_text", read), \
                    mock.patch.object(Path, "iterdir", return_value=iter(())), \
                    mock.patch.object(sys, "argv", ["check.py", "--root", str(root)]), \
                    contextlib.redirect_stderr(io.StringIO()) as errors:
                result = gate.main()
            return result, errors.getvalue()

    def test_undeclared_fetch_in_root_and_cmake_directory(self):
        for filename in ("CMakeLists.txt", "cmake/Dependencies.cmake", "libs/test/CMakeLists.txt"):
            with self.subTest(filename=filename):
                result, errors = self.check_fixture(filename, "FetchContent_Declare(undeclared)\n")
                self.assertEqual(result, 1)
                self.assertIn("fetches 'undeclared'", errors)

    def test_forbidden_spdx_in_root_build_file(self):
        result, errors = self.check_fixture("CMakeLists.txt", "# SPDX-License-" + "Identifier: GPL-3.0\n")
        self.assertEqual(result, 1)
        self.assertIn("SPDX identifier", errors)

    def test_script_and_workflow_licenses_are_checked(self):
        for filename in ("tools/package/install.sh.in", "tools/package/windows.nsi",
                         "tools/ci/example.sh", "tools/ci/example.ps1",
                         ".github/workflows/example.yml", ".github/workflows/example.yaml",
                         ".github/scripts/example.sh", "plugins/common/example.sh"):
            with self.subTest(filename=filename):
                result, errors = self.check_fixture(
                    filename, "# SPDX-License-" + "Identifier: AGPL-3.0-only\n")
                self.assertEqual(result, 1)
                self.assertIn("SPDX identifier", errors)

    def test_only_the_named_file_may_name_a_gpl_tag(self):
        tag = 'string(REPLACE "SPDX-License-' + 'Identifier: MIT" "SPDX-License-' + 'Identifier: GPL-3.0-or-later")\n'
        result, errors = self.check_fixture("plugins/template/framework/tools/new_product.cmake", tag)
        self.assertEqual(result, 0, errors)
        result, errors = self.check_fixture("plugins/template/framework/tools/other.cmake", tag)
        self.assertEqual(result, 1)
        self.assertIn("SPDX identifier", errors)

    def test_local_plugin_projects_are_first_party(self):
        local = ('ExternalProject_Add(plugin-${dir}\n  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"\n)\n',
                 'ExternalProject_Add(plugin-${name}\n  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/template"\n)\n')
        for text in local:
            with self.subTest(text=text):
                result, errors = self.check_fixture("plugins/CMakeLists.txt", text)
                self.assertEqual(result, 0, errors)
                result, errors = self.check_fixture("libs/test/CMakeLists.txt", text)
                self.assertEqual(result, 1)
        for text in ('ExternalProject_Add(plugin-${name}\n  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../x"\n)\n',
                     'ExternalProject_Add(plugin-${name}\n  URL https://example.com/x.tar.gz\n)\n'):
            with self.subTest(text=text):
                result, errors = self.check_fixture("plugins/CMakeLists.txt", text)
                self.assertEqual(result, 1)
                self.assertIn("fetches 'plugin-'", errors)

    def test_cmake_commands_are_case_insensitive(self):
        for command in ("fetchcontent_declare", "externalproject_add"):
            with self.subTest(command=command):
                result, errors = self.check_fixture("libs/test/CMakeLists.txt", command + "(undeclared)\n")
                self.assertEqual(result, 1)
                self.assertIn("fetches 'undeclared'", errors)

    def test_gpl_daw_and_mit_plugin_boundaries(self):
        for filename, identifier, expected in (
            ("daw/example.cpp", "GPL-3.0-or-later", 0),
            ("plugins/common/example.cpp", "GPL-3.0-or-later", 1),
            ("plugins/common/example.cpp", "MIT OR GPL-3.0-or-later", 1),
            ("libs/core-analysis/example.cpp", "GPL-3.0-or-later", 1),
            ("libs/core-analysis/example.cpp", "MIT", 0),
            ("plugins/suite/export/product/Catalog.cpp.in", "@LICENSE@", 0),
            ("plugins/common/example.cpp", "@LICENSE@", 1),
            ("daw/example.cpp", "AGPL-3.0-or-later", 1),
        ):
            with self.subTest(filename=filename, identifier=identifier):
                result, errors = self.check_fixture(filename, "// SPDX-License-" + "Identifier: " + identifier + "\n")
                self.assertEqual(result, expected, errors)


class ComponentPaths(unittest.TestCase):
    def test_native_windows_paths_keep_the_same_license_boundary(self):
        for path, expected in (("cmake/product.h.in", True),
                               ("libs/core-base/src/files.cpp", True),
                               ("plugins/suite/compiler/src/codegen.cpp", True),
                               ("libs/command-bus/src/bus.cpp", False),
                               ("host/engine/src/app.cpp", False)):
            with self.subTest(path=path):
                self.assertEqual(gate.mit_component(PureWindowsPath(path)), expected)


class DependencyScopes(unittest.TestCase):
    def test_gpl_requires_explicit_daw_only_scope(self):
        for scope in (None, "plugins", "daw", "daw-only"):
            for license_id in ("GPL-3.0-only", "GPL-3.0-or-later"):
                with self.subTest(scope=scope, license_id=license_id):
                    dependency = {"license": license_id, "scope": scope}
                    self.assertEqual(gate.dependency_license_ok(dependency, {"MIT", license_id}), scope == "daw-only")

    def test_or_alternative_cannot_hide_plugin_gpl(self):
        self.assertFalse(gate.dependency_license_ok({"license": "MIT OR GPL-3.0-only", "scope": "plugins"}, {"MIT"}))

    def test_lame_is_the_named_lgpl_exception(self):
        lame = {"name": "lame", "license": "LGPL-2.0-or-later", "scope": "daw-only"}
        self.assertTrue(gate.dependency_license_ok(lame, {"MIT"}))
        self.assertFalse(gate.dependency_license_ok(dict(lame, scope="plugins"), {"MIT"}))
        self.assertFalse(gate.dependency_license_ok(dict(lame, name="other"), {"MIT"}))
        self.assertFalse(gate.dependency_license_ok(dict(lame, license="LGPL-3.0-only"), {"MIT"}))

    def test_other_copyleft_is_not_approved_by_daw_scope(self):
        for license_id in ("AGPL-3.0-only", "LGPL-3.0-only", "GPL-2.0-only"):
            self.assertFalse(gate.dependency_license_ok({"license": license_id, "scope": "daw-only"}, {"MIT"}))


class Expressions(unittest.TestCase):
    allowed = {"MIT", "BSD-3-Clause", "Apache-2.0", "Apache-2.0 WITH LLVM-exception"}

    def test_precedence_and_parentheses(self):
        for expression, expected in (
            ("MIT", True), ("GPL-3.0 AND (MIT OR BSD-3-Clause)", False),
            ("(GPL-3.0 AND MIT) OR BSD-3-Clause", True),
            ("GPL-3.0 AND MIT OR BSD-3-Clause", True),
            ("GPL-3.0 AND (MIT OR GPL-3.0)", False),
            ("((MIT))", True), ("MIT AND(BSD-3-Clause OR GPL-3.0)", True),
            ("MIT OR GPL-3.0 AND BSD-3-Clause", True),
        ):
            with self.subTest(expression=expression):
                self.assertEqual(gate.license_ok(expression, self.allowed), expected)

    def test_with_requires_an_allowed_complete_term(self):
        self.assertTrue(gate.license_ok("Apache-2.0 WITH LLVM-exception", self.allowed))
        self.assertFalse(gate.license_ok("MIT WITH Unknown-exception", self.allowed))
        self.assertFalse(gate.license_ok("GPL-3.0 WITH LLVM-exception", self.allowed))

    def test_malformed_syntax_is_rejected_even_in_allowed_alternatives(self):
        for expression in ("", "()", "MIT OR", "OR MIT", "MIT AND", "MIT MIT", "(MIT",
                           "MIT)", "MIT OR (BSD-3-Clause", "MIT OR )BSD-3-Clause(",
                           "MIT OR OR BSD-3-Clause", "MIT WITH", "MIT WITH X WITH Y",
                           "(MIT OR BSD-3-Clause) WITH X", "MIT OR GPL-3.0 garbage",
                           "MIT OR GPL-3.0@", "MIT or BSD-3-Clause",
                           "MIT OR GPL+3.0", "MIT OR foo:bar", "MIT OR LicenseRef-x+"):
            with self.subTest(expression=expression):
                self.assertFalse(gate.license_ok(expression, self.allowed))

    def test_current_manifest_expressions(self):
        manifest = json.loads((Path(__file__).resolve().parents[2] / "third_party/manifest.json").read_text())
        for dependency in manifest["dependencies"]:
            with self.subTest(dependency=dependency["name"]):
                self.assertTrue(gate.dependency_license_ok(dependency, set(manifest["allowed_licenses"])))


if __name__ == "__main__":
    unittest.main()
