#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""License gate for the monorepo.

Fails (exit 1) when:
  * a directory under third_party/ has no manifest entry, or vice versa;
  * a manifest entry's license is not on the allow-list ("A OR B" passes
    when at least one alternative is allowed; "A AND B" requires all);
  * a declared license file is missing;
  * a dependency is not mentioned in THIRD_PARTY.md;
  * a CMake FetchContent_Declare / ExternalProject_Add names a dependency that
    is not in the manifest;
  * GPL dependencies are not declared DAW-only, or plugin code carries GPL;
  * first-party source carries an incompatible SPDX identifier.

Run: tools/license-check/check.py --root <repo>
"""
import argparse
import json
import pathlib
import re
import sys

FIRST_PARTY_DIRS = ["cmake", "libs", "host", "plugins", "modules", "daw", "mcp", "tools", "tests", "skill", "docs",
                    ".github"]
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".m", ".mm", ".py", ".lua", ".cmake",
                   ".sh", ".ps1", ".nsi", ".yml", ".yaml"}
# Keep GPL approvals separate from the permissive dependency allow-list.
GPL_LICENSES = {"GPL-3.0-only", "GPL-3.0-or-later"}
# LGPL approved by name, DAW-only (ADR-0055 §8): LAME, statically linked into
# the GPL DAW. Any other LGPL dependency still needs the maintainer's approval.
APPROVED_LGPL = {"lame": "LGPL-2.0-or-later"}
MIT_LIBRARIES = {"core-base", "core-json", "core-schema", "core-params",
                 "core-patch", "core-dsp", "core-analysis"}
SPDX_RE = re.compile("SPDX-License-" + r"Identifier:[ \t]*([^\n]+)")


def mit_component(rel):
    rel = pathlib.PurePosixPath(rel)
    parts = rel.parts
    return (parts[0] in ("plugins", "modules") or  # modules/: ADR-0062 §9
            (len(parts) > 1 and parts[0] == "libs" and parts[1] in MIT_LIBRARIES) or
            str(rel) == "cmake/product.h.in")


def dependency_license_ok(dependency, allowed):
    expression = dependency["license"]
    # Reject GPL even in an OR alternative unless the whole component is
    # explicitly restricted to the DAW. A plugin scope must never be GPL.
    tokens = re.findall(r"[A-Za-z0-9][A-Za-z0-9.+:-]*", expression)
    copyleft = any(re.match(r"(?:A?GPL|LGPL)(?:-|$)", t) for t in tokens)
    scope = dependency.get("scope")
    if copyleft and scope != "daw-only":
        return False
    permitted = set(allowed) - GPL_LICENSES
    if scope == "daw-only":
        permitted |= GPL_LICENSES
        approved = APPROVED_LGPL.get(dependency.get("name"))
        if approved:
            permitted.add(approved)
    return license_ok(expression, permitted)


FETCH_RE = re.compile(r"(?:FetchContent_Declare|ExternalProject_Add)\s*\(\s*([A-Za-z0-9_.+-]+)", re.IGNORECASE)
# What may follow "ExternalProject_Add(plugin-" in plugins/CMakeLists.txt: a
# variable name and a SOURCE_DIR that is a folder of plugins/ (no "..").
LOCAL_PLUGIN_RE = re.compile(r'\$\{\w+\}\s+SOURCE_DIR\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}'
                             r'(?:/(?:[\w-]+|\$\{\w+\}))+"')
SKIP_PARTS = {"build", "_deps", ".git", "third_party", "node_modules"}
# Files that name a GPL licence without being under it. The plugin template's
# product generator offers GPL-3.0-or-later as a licence for the product it
# writes, and rewrites the SPDX tags of the copies it makes.
SPDX_NAMES_ONLY = {pathlib.PurePosixPath("plugins/template/framework/tools/new_product.cmake")}


def license_ok(expr: str, allowed: set) -> bool:
    # Parse the whole expression before evaluating: an allowed OR alternative
    # must never hide malformed syntax. WITH terms need explicit approval as a
    # complete license/exception pair, not just approval of the base license.
    tokens = re.findall(r"[A-Za-z0-9][A-Za-z0-9.+:-]*|[()]|[^\s]", expr)
    at = 0

    def take(token):
        nonlocal at
        if at < len(tokens) and tokens[at] == token:
            at += 1
            return True
        return False

    def identifier(exception=False):
        nonlocal at
        if at >= len(tokens) or tokens[at] in {"AND", "OR", "WITH"}:
            raise ValueError("expected SPDX identifier")
        token = tokens[at]
        if exception:
            pattern = r"[A-Za-z0-9][A-Za-z0-9.-]*"
        elif token.startswith(("LicenseRef-", "DocumentRef-")):
            pattern = r"(?:DocumentRef-[A-Za-z0-9.-]+:)?LicenseRef-[A-Za-z0-9.-]+"
        else:
            pattern = r"[A-Za-z0-9][A-Za-z0-9.-]*\+?"
        if not re.fullmatch(pattern, token):
            raise ValueError("invalid SPDX identifier")
        at += 1
        return token

    def primary():
        if take("("):
            value = disjunction()
            if not take(")"):
                raise ValueError("missing closing parenthesis")
            return value
        term = identifier()
        if take("WITH"):
            term += " WITH " + identifier(exception=True)
        return term in allowed

    def conjunction():
        value = primary()
        while take("AND"):
            right = primary()
            value = value and right
        return value

    def disjunction():
        value = conjunction()
        while take("OR"):
            right = conjunction()
            value = value or right
        return value

    try:
        value = disjunction()
        return at == len(tokens) and value
    except (ValueError, RecursionError):
        return False


def first_party_files(root: pathlib.Path):
    if (root / "CMakeLists.txt").is_file():
        yield root / "CMakeLists.txt"
    for d in FIRST_PARTY_DIRS:
        base = root / d
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if not p.is_file() or SKIP_PARTS.intersection(p.relative_to(root).parts):
                continue
            yield p


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[2]))
    args = ap.parse_args()
    root = pathlib.Path(args.root).resolve()
    errors = []

    manifest_path = root / "third_party" / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    allowed = set(manifest["allowed_licenses"])
    deps = manifest["dependencies"]
    by_dir = {d["dir"]: d for d in deps if d.get("dir")}
    names = {d["name"].lower() for d in deps}
    third_party_md = (root / "THIRD_PARTY.md").read_text() if (root / "THIRD_PARTY.md").exists() else ""
    if not third_party_md:
        errors.append("THIRD_PARTY.md is missing")

    tp = root / "third_party"
    for child in sorted(tp.iterdir()):
        if child.is_dir() and child.name not in by_dir:
            errors.append(f"third_party/{child.name}: no entry in third_party/manifest.json")

    for d in deps:
        tag = d["name"]
        if not dependency_license_ok(d, allowed):
            errors.append(f"{tag}: license '{d['license']}' is not on the allow-list "
                          f"(GPL requires scope: daw-only; plugins cannot contain GPL)")
        if d.get("dir") and not (tp / d["dir"]).is_dir():
            errors.append(f"{tag}: manifest dir third_party/{d['dir']} does not exist")
        lf = d.get("license_file")
        if lf and not (tp / lf).is_file():
            errors.append(f"{tag}: license file third_party/{lf} is missing")
        if d.get("fetched"):
            # Fetched at configure time on some platforms only (never in the
            # default offline build): must still declare license and pin.
            if not d.get("commit") or not d.get("source"):
                errors.append(f"{tag}: fetched dependency needs 'source' and a pinned 'commit'")
        elif not lf and d.get("linkage") != "system":
            errors.append(f"{tag}: no license_file declared")
        if tag.lower() not in third_party_md.lower():
            errors.append(f"{tag}: not described in THIRD_PARTY.md")

    for p in first_party_files(root):
        rel = p.relative_to(root)
        suffix = p.with_suffix("").suffix if p.suffix == ".in" else p.suffix
        if suffix in SOURCE_SUFFIXES or p.name == "CMakeLists.txt":
            try:
                text = p.read_text(errors="replace")
            except OSError:
                continue
            if pathlib.PurePosixPath(rel.as_posix()) not in SPDX_NAMES_ONLY:
                for match in SPDX_RE.finditer(text):
                    identifier = match.group(1).split("*/", 1)[0].strip()
                    if identifier == "@LICENSE@" and rel.parts[:3] == ("plugins", "suite", "export"):
                        continue
                    permitted = (set(allowed) - GPL_LICENSES if mit_component(rel)
                                 else {"GPL-3.0-or-later"})
                    if (mit_component(rel) and re.search(r"\b(?:A?GPL|LGPL)-", identifier)
                            or not license_ok(identifier, permitted)):
                        errors.append(f"{rel}: incompatible SPDX identifier '{identifier}'")
            if p.name == "CMakeLists.txt" or p.suffix == ".cmake":
                for m in FETCH_RE.finditer(text):
                    dep = m.group(1).lower()
                    # Template-derived plugins are local first-party sources
                    # (a plugin's folder, or the template's tree for a
                    # product kept elsewhere in plugins/), built in isolation
                    # to avoid CMake target collisions.
                    if (dep == "plugin-" and rel == pathlib.Path("plugins/CMakeLists.txt")
                            and LOCAL_PLUGIN_RE.match(text, m.end())):
                        continue
                    if dep not in names:
                        errors.append(f"{rel}: fetches '{m.group(1)}', which is not in third_party/manifest.json")

    if errors:
        print("license-check: FAILED", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1
    print(f"license-check: OK ({len(deps)} dependencies)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
