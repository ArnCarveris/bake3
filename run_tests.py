#!/usr/bin/env python3

import json
import os
import platform
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


EXE_SUFFIX = ".exe" if platform.system() == "Windows" else ""
OBJ_SUFFIX = ".obj" if platform.system() == "Windows" else ".o"

ANSI_ESCAPE_RE = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
SUMMARY_RE = re.compile(
    r"applications:\s*(?P<applications>\d+),\s*packages:\s*(?P<packages>\d+),\s*templates:\s*(?P<templates>\d+)"
)
LIST_ENTRY_RE = re.compile(r"^(?P<kind>[APT])\s+(?P<name>.+?)\s+=>")


@dataclass(frozen=True)
class ListState:
    applications: int
    packages: int
    templates: int
    application_names: frozenset[str]
    package_names: frozenset[str]
    template_names: frozenset[str]


class BakeTests(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.repo_root = Path(__file__).resolve().parent
        cls.bake_bin = cls.repo_root / "build" / f"bake{EXE_SUFFIX}"
        cls.bake_home = cls.repo_root / "test" / "tmp" / "bake_home"
        cls.env = os.environ.copy()
        cls.env["BAKE_HOME"] = str(cls.bake_home)

        cls._require_supported_os()
        cls.bake_home.parent.mkdir(parents=True, exist_ok=True)
        cls.git_snapshot_before = cls.git_snapshot()

        cls._remove_stale_build_state()
        cls.run_cmd(["make", "clean"])
        cls.run_cmd(["make", "-j", "8"])
        cls.bake(["setup", "--local"])

    @classmethod
    def _remove_stale_build_state(cls) -> None:
        """Delete .bake build dirs left by previous runs or manual builds.

        Local trees accumulate incremental state built by other bake binaries
        or with other flags; tests must start from the same blank slate CI
        gets from a fresh checkout. Nothing under these roots is tracked by
        git, so removal is safe.
        """
        # Limited to trees the suite builds; other integration projects (e.g.
        # flecs-engine) keep expensive local bundle caches under .bake that
        # the suite never uses.
        for root in ("projects", "tests", os.path.join("integration", "flecs-modules-test")):
            base = cls.repo_root / "test" / root
            if not base.is_dir():
                continue
            for bake_dir in base.glob("**/.bake"):
                if bake_dir.is_dir() and not bake_dir.is_symlink():
                    shutil.rmtree(bake_dir)

    def setUp(self) -> None:
        self.tmp_root = self.repo_root / "test" / "tmp"
        self.tmp_root.mkdir(parents=True, exist_ok=True)
        self._tmp_entries_before = {entry.name for entry in self.tmp_root.iterdir()}
        self.bake(["clean", "test"])
        self.bake(["reset"])
        self.assert_empty_list_state()

    def tearDown(self) -> None:
        """Remove everything the test created under test/tmp.

        A local checkout otherwise accumulates one workspace per test per run,
        and diverges from the empty test/tmp that CI starts every job with.
        Set BAKE_TEST_KEEP_TMP=1 to keep the trees for debugging.
        """
        if os.environ.get("BAKE_TEST_KEEP_TMP"):
            return

        before = getattr(self, "_tmp_entries_before", None)
        if before is None or not self.tmp_root.is_dir():
            return

        for entry in self.tmp_root.iterdir():
            if entry.name in before:
                continue
            if entry.is_dir():
                self._rm_tree(entry)
            else:
                entry.unlink(missing_ok=True)

    @classmethod
    def _require_supported_os(cls) -> None:
        if platform.system() not in {"Linux", "Darwin", "Windows"}:
            raise unittest.SkipTest("run_tests.py supports Linux, macOS and Windows only")

    @classmethod
    def run_cmd(
        cls,
        args: list[str],
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
    ) -> str:
        run_cwd = str(cwd if cwd is not None else cls.repo_root)
        proc = subprocess.run(
            args,
            cwd=run_cwd,
            env=env if env is not None else cls.env,
            text=True,
            capture_output=True,
            check=False,
        )

        output = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            raise AssertionError(
                "Command failed with non-zero exit status\n"
                f"cwd: {run_cwd}\n"
                f"cmd: {' '.join(args)}\n"
                f"exit code: {proc.returncode}\n"
                f"output:\n{output}"
            )
        return output

    @classmethod
    def bake(
        cls,
        args: list[str],
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
    ) -> str:
        return cls.run_cmd([str(cls.bake_bin), *args], cwd=cwd, env=env)

    @classmethod
    def bake_expect_failure(
        cls,
        args: list[str],
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
    ) -> str:
        run_cwd = str(cwd if cwd is not None else cls.repo_root)
        proc = subprocess.run(
            [str(cls.bake_bin), *args],
            cwd=run_cwd,
            env=env if env is not None else cls.env,
            text=True,
            capture_output=True,
            check=False,
        )

        output = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode == 0:
            raise AssertionError(
                "Expected command to fail but it succeeded\n"
                f"cwd: {run_cwd}\n"
                f"cmd: {str(cls.bake_bin)} {' '.join(args)}\n"
                f"output:\n{output}"
            )

        return output

    @staticmethod
    def strip_ansi(text: str) -> str:
        return ANSI_ESCAPE_RE.sub("", text)

    @staticmethod
    def strict_link_warning_flag() -> str:
        if platform.system() == "Darwin":
            return "-Wl,-fatal_warnings"
        return "-Wl,--fatal-warnings"

    def artefact_path(self, target: str) -> Path:
        project_dir = self.repo_root / target
        info = self.strip_ansi(self.bake(["info", str(project_dir)], cwd=project_dir))

        kind_match = re.search(r"^kind:\s*(.+)$", info, re.MULTILINE)
        output_match = re.search(r"^output:\s*(.+)$", info, re.MULTILINE)
        path_match = re.search(r"^path:\s*(.+)$", info, re.MULTILINE)
        if not kind_match or not output_match or not path_match:
            raise AssertionError(f"Failed to parse bake info output:\n{info}")

        kind = kind_match.group(1).strip()
        output_name = output_match.group(1).strip()
        project_path = Path(path_match.group(1).strip())

        if kind == "package":
            artefact_name = f"{output_name}.lib" if platform.system() == "Windows" else f"lib{output_name}.a"
        else:
            artefact_name = f"{output_name}.exe" if platform.system() == "Windows" else output_name

        matches = sorted(project_path.glob(f".bake/*/{artefact_name}"))
        if not matches:
            raise AssertionError(
                f"Could not find artefact '{artefact_name}' for target '{target}' under {project_path / '.bake'}"
            )

        return max(matches, key=lambda p: p.stat().st_mtime_ns)

    def object_paths(self, target: str) -> list[Path]:
        project_dir = self.repo_root / target
        obj_dirs = sorted(project_dir.glob(".bake/*/obj"))
        if not obj_dirs:
            raise AssertionError(f"Could not find object directory for target '{target}'")

        obj_dir = max(obj_dirs, key=lambda p: p.stat().st_mtime_ns)
        obj_ext = ".obj" if platform.system() == "Windows" else ".o"
        objects = sorted(p for p in obj_dir.rglob(f"*{obj_ext}") if p.is_file())
        if not objects:
            raise AssertionError(f"Could not find object files in {obj_dir}")

        return objects

    @classmethod
    def list_state(cls, cwd: Path | None = None) -> ListState:
        raw = cls.bake(["list"], cwd=cwd)
        text = cls.strip_ansi(raw)

        apps: set[str] = set()
        packages: set[str] = set()
        templates: set[str] = set()

        for line in text.splitlines():
            entry_match = LIST_ENTRY_RE.match(line.strip())
            if not entry_match:
                continue

            kind = entry_match.group("kind")
            name = entry_match.group("name").strip()
            if kind == "A":
                apps.add(name)
            elif kind == "P":
                packages.add(name)
            elif kind == "T":
                templates.add(name)

        summary_match = SUMMARY_RE.search(text)
        if not summary_match:
            raise AssertionError(f"Failed to parse bake list summary:\n{text}")

        return ListState(
            applications=int(summary_match.group("applications")),
            packages=int(summary_match.group("packages")),
            templates=int(summary_match.group("templates")),
            application_names=frozenset(apps),
            package_names=frozenset(packages),
            template_names=frozenset(templates),
        )

    def assert_empty_list_state(self, cwd: Path | None = None) -> None:
        state = self.list_state(cwd=cwd)
        self.assertEqual(
            (state.applications, state.packages, state.templates),
            (0, 0, 0),
            f"Expected empty bake environment, got: {state}",
        )
        self.assertEqual(state.application_names, frozenset())
        self.assertEqual(state.package_names, frozenset())
        self.assertEqual(state.template_names, frozenset())

    @classmethod
    def git_snapshot(cls) -> tuple[str, ...]:
        output = cls.run_cmd(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=cls.repo_root,
        )
        lines: Iterable[str] = (line for line in output.splitlines() if line.strip())
        return tuple(sorted(lines))

    def test_build_test_target(self) -> None:
        self.bake(["build", "test"])
        state = self.list_state()
        self.assertGreater(state.applications + state.packages + state.templates, 0)
        self.assertIn("flecs", state.package_names)
        self.assertIn("flecs.components.graphics", state.package_names)

    def test_info_accepts_relative_target(self) -> None:
        project_dir = self.repo_root / "test" / "projects" / "c" / "app_helloworld"
        info = self.strip_ansi(self.bake(["info", "."], cwd=project_dir))
        self.assertRegex(info, r"(?m)^id:\s+examples\.c\.app_helloworld$")
        self.assertRegex(info, r"(?m)^kind:\s+application$")

    def test_build_projects_target(self) -> None:
        self.bake(["build", "test/projects"])
        state = self.list_state()
        self.assertIn("examples.c.app_clib", state.application_names)
        self.assertIn("examples.c.pkg_helloworld", state.package_names)

    def test_unit_helpers(self) -> None:
        """Run the C unit tests over bake's pure helpers.

        Path, string and list helpers are only reached indirectly by the
        end-to-end tests, which report their edge cases as unrelated build
        failures.
        """
        self.run_cmd(["make", "unit"])
        unit_bin = self.repo_root / "build" / f"bake_unit_tests{EXE_SUFFIX}"
        output = self.run_cmd([str(unit_bin)])
        self.assertIn("0 failures", output, output)

    def test_amalgamate_list_format_supports_prefix_and_disable_flags(self) -> None:
        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)

        self.bake(["build", target])

        # First config: default name, no disable-flags -> full output preserved
        full_header = (distr / "examples_c_pkg_amalgamate_disable.h").read_text()
        self.assertIn("#ifdef EXAMPLES_FEATURE_REMOVED", full_header)
        self.assertIn("#if defined(EXAMPLES_FLAG_OFF)", full_header)
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_EXTRA", full_header)
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (1)", full_header)

        # Second config: prefix "mini" + disable-flags -> separate, stripped files
        mini_header = (distr / "mini.h").read_text()
        mini_source = (distr / "mini.c").read_text()

        # Simple #ifdef / #if defined guards on disabled flags are stripped,
        # including the addon-style "#define FLAG" enable line (so the flag is
        # genuinely undefined for the compiler).
        self.assertNotIn("EXAMPLES_FEATURE_REMOVED", mini_header)
        self.assertNotIn("removed_decl", mini_header)
        self.assertNotIn("flag_only", mini_header)
        self.assertNotIn("EXAMPLES_FLAG_OFF", mini_header)
        self.assertNotIn("#define EXAMPLES_FLAG_OFF", mini_header)
        self.assertNotIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_EXTRA", mini_header)
        self.assertNotIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_NESTED", mini_header)
        self.assertNotIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (1)", mini_header)
        self.assertNotIn("EXAMPLES_FEATURE_REMOVED", mini_source)
        self.assertNotIn("removed_decl", mini_source)

        # #else branch kept, #ifndef body kept, non-disabled flags preserved
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (2)", mini_header)
        self.assertIn("examples_c_pkg_amalgamate_disable_kept_decl", mini_header)
        self.assertIn("#ifdef EXAMPLES_KEEP_THIS", mini_header)
        self.assertIn("examples_c_pkg_amalgamate_disable_other_decl", mini_header)
        self.assertIn("examples_c_pkg_amalgamate_disable_value", mini_header)
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_BIAS (10)", mini_header)
        self.assertIn("#define EXAMPLES_DEBUG_INFO", mini_header)

        # The prefixed source includes its own prefixed header
        self.assertIn('#include "mini.h"', mini_source)

    def test_amalgamate_disabled_flags_produce_compilable_output(self) -> None:
        # Regression: a disabled flag's enable "#define FLAG" used to be left in
        # the output, so the compiler saw the flag as defined and activated
        # compound guards (e.g. "#if defined(X) && defined(FLAG)") whose
        # declarations had been stripped -> dangling references that fail to
        # compile. The enable #define must be stripped so the flag is undefined.
        compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            self.skipTest("no C compiler available")

        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)
        self.bake(["build", target])

        for name in ("mini.c", "examples_c_pkg_amalgamate_disable.c"):
            obj = distr / (name + ".o")
            self.run_cmd([compiler, "-c", str(distr / name), "-o", str(obj)])
            self.assertTrue(obj.exists(), f"expected {name} to compile")
            obj.unlink()

    def test_amalgamate_output_is_cleaned(self) -> None:
        # The amalgamated output drops comment blocks containing an @file
        # directive and collapses runs of blank lines down to a single blank
        # line. A blank line that terminates a backslash-continued macro is
        # significant; capping at one blank line preserves it (collapsing it
        # away would splice the next line into the macro).
        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)
        self.bake(["build", target])

        for name in (
            "examples_c_pkg_amalgamate_disable.h",
            "examples_c_pkg_amalgamate_disable.c",
            "mini.h",
            "mini.c",
        ):
            text = (distr / name).read_text()
            self.assertNotIn("@file", text)
            self.assertNotIn("\n\n\n", text)

    def test_amalgamate_regenerates_when_output_is_stale(self) -> None:
        # Regression: amalgamation used to skip regeneration when the existing
        # output was newer than the source files (mtime heuristic). A stale or
        # incorrect output (e.g. from an older binary, or after a config change
        # that left source mtimes untouched) would never be rewritten. The
        # replacement must be content-based instead.
        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)
        self.bake(["build", target])

        mini = distr / "mini.h"
        stripped = mini.read_text()
        self.assertNotIn("EXAMPLES_FLAG_OFF", stripped)

        # Overwrite the stripped output with the full (unstripped) header and
        # make it newer than every source file.
        full = (distr / "examples_c_pkg_amalgamate_disable.h").read_text()
        mini.write_text(full)
        os.utime(mini, None)
        self.assertIn("EXAMPLES_FLAG_OFF", mini.read_text())

        # Rebuilding without deleting must restore the stripped output.
        self.bake(["build", target])
        regenerated = mini.read_text()
        self.assertNotIn("EXAMPLES_FLAG_OFF", regenerated)
        self.assertNotIn("EXAMPLES_FEATURE_REMOVED", regenerated)
        self.assertEqual(regenerated, stripped)

    def test_build_distinguishes_sources_with_colliding_flat_names(self) -> None:
        self.bake(["build", "test/projects/c/app_obj_collision"])
        state = self.list_state()
        self.assertIn("examples.c.app_obj_collision", state.application_names)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_strict_build_enables_extended_c_warning_flags(self) -> None:
        output = self.strip_ansi(
            self.bake(["--trace", "--strict", "build", "test/projects/c/app_helloworld"])
        )

        self.assertIn("-Wall", output)
        self.assertIn("-Wextra", output)
        self.assertIn("-Wcast-align", output)
        self.assertIn("-Wformat=2", output)
        self.assertIn("-Wmissing-prototypes", output)
        self.assertIn("-Wstrict-prototypes", output)
        self.assertIn("-Wold-style-definition", output)
        self.assertIn(self.strict_link_warning_flag(), output)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_strict_build_enables_extended_cpp_warning_flags(self) -> None:
        output = self.strip_ansi(
            self.bake(["--trace", "--strict", "build", "test/projects/cpp/app_helloworld"])
        )

        self.assertIn("-Wall", output)
        self.assertIn("-Wextra", output)
        self.assertIn("-Wcast-align", output)
        self.assertIn("-Wformat=2", output)
        self.assertIn("-Wnon-virtual-dtor", output)
        self.assertIn("-Wold-style-cast", output)
        self.assertIn("-Woverloaded-virtual", output)
        self.assertIn("-Wzero-as-null-pointer-constant", output)
        self.assertIn(self.strict_link_warning_flag(), output)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style -l flags; bake defaults to MSVC on Windows")
    def test_strict_build_avoids_duplicate_dependency_link_inputs(self) -> None:
        self.bake(["build", "test/projects/envpkgs/libmath"])
        output = self.strip_ansi(
            self.bake(["--trace", "--strict", "build", "test/projects/ws/apps/use_env"])
        )

        self.assertIn(self.strict_link_warning_flag(), output)
        self.assertNotIn(" -lenvmath -lenvmath", output)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_default_c_standard_keeps_posix_declarations_visible(self) -> None:
        """POSIX APIs stay declared under bake's default C standard.

        glibc suppresses everything outside the C standard when a strict
        -std=cNN defines __STRICT_ANSI__, which silently removes
        clock_gettime and the NI_* constants from an otherwise ordinary
        build.
        """
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"posix_vis_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            f'{{"id": "posix_vis_{stamp}", "type": "application"}}\n'
        )
        (root / "src" / "main.c").write_text(
            "#include <netdb.h>\n"
            "#include <time.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            "    struct timespec ts;\n"
            "    clock_gettime(CLOCK_MONOTONIC, &ts);\n"
            "    printf(\"flags=%d\\n\", NI_NUMERICHOST | NI_NUMERICSERV);\n"
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(self.bake(["run", str(root)]))
        self.assertIn("flags=", output)

    @unittest.skipIf(platform.system() != "Linux", "_DEFAULT_SOURCE only applies to glibc")
    def test_explicit_feature_test_macro_is_not_widened(self) -> None:
        """A project that selects its own glibc feature test macro keeps it.

        Adding _DEFAULT_SOURCE on top of an explicit _POSIX_C_SOURCE would
        expose BSD and SVID declarations the project deliberately excluded.
        """
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"feature_macro_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            f'{{"id": "feature_macro_{stamp}", "type": "application", '
            '"lang.c": {"defines": ["_POSIX_C_SOURCE=200809L"]}}\n'
        )
        (root / "src" / "main.c").write_text("int main(void) { return 0; }\n")

        output = self.strip_ansi(self.bake(["--trace", "build", str(root)]))
        self.assertIn("-D_POSIX_C_SOURCE=200809L", output)
        self.assertNotIn("-D_DEFAULT_SOURCE", output)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_lang_c_flag_keys_reach_the_compiler_and_linker(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"lang_keys_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "extra").mkdir(parents=True)
        rpath = (root / "rpath").as_posix()
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "lang_keys_{stamp}",\n'
            '    "type": "application",\n'
            '    "lang.c": {\n'
            '        "cflags": ["-DFROM_CFLAGS"],\n'
            '        "defines": ["FROM_DEFINES"],\n'
            '        "c-standard": "c11",\n'
            f'        "include": ["{(root / "extra").as_posix()}"],\n'
            f'        "libpath": ["{(root / "extra").as_posix()}"],\n'
            '        "lib": ["m"],\n'
            f'        "ldflags": ["-Wl,-rpath,{rpath}"]\n'
            "    }\n"
            "}\n"
        )
        (root / "extra" / "extra.h").write_text("#define EXTRA_VALUE 7\n")
        (root / "src" / "main.c").write_text(
            "#include <extra.h>\n"
            "#include <math.h>\n"
            "#include <stdio.h>\n"
            "#ifndef FROM_CFLAGS\n"
            '#error "cflags did not reach the compiler"\n'
            "#endif\n"
            "#ifndef FROM_DEFINES\n"
            '#error "defines did not reach the compiler"\n'
            "#endif\n"
            "int main(void) {\n"
            '    printf("value=%d\\n", EXTRA_VALUE + (int)sqrt(4.0));\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(self.bake(["--trace", "run", str(root)]))

        self.assertIn("value=9", output)
        self.assertIn("-DFROM_CFLAGS", output)
        self.assertIn("-DFROM_DEFINES", output)
        self.assertIn("-std=c11", output)
        self.assertIn(f'-I"{(root / "extra").as_posix()}"', output)
        self.assertIn(f'-L"{(root / "extra").as_posix()}"', output)
        self.assertIn(" -lm", output)
        self.assertIn(f"-Wl,-rpath,{rpath}", output)

    def test_relative_include_path_resolves_against_the_project(self) -> None:
        """A relative 'include' path is relative to the project directory.

        The compiler runs with bake's working directory, not the project's, so
        a path passed through verbatim resolves against whatever directory
        bake happened to be started from.
        """
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"rel_include_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "vendor").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "rel_include_{stamp}",\n'
            '    "type": "application",\n'
            '    "lang.c": {"include": ["vendor"]}\n'
            "}\n"
        )
        (root / "vendor" / "vendored.h").write_text("#define VENDORED_VALUE 5\n")
        (root / "src" / "main.c").write_text(
            "#include <vendored.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("value=%d\\n", VENDORED_VALUE);\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(self.bake(["run", str(root)], cwd=self.repo_root))
        self.assertIn("value=5", output)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_lang_cpp_flag_keys_reach_the_cpp_compiler(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"lang_cpp_keys_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "lang_cpp_keys_{stamp}",\n'
            '    "type": "application",\n'
            '    "value": {"language": "cpp"},\n'
            '    "lang.cpp": {\n'
            '        "cxxflags": ["-DFROM_CXXFLAGS"],\n'
            '        "cpp-standard": "c++17"\n'
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.cpp").write_text(
            "#include <cstdio>\n"
            "#ifndef FROM_CXXFLAGS\n"
            '#error "cxxflags did not reach the compiler"\n'
            "#endif\n"
            "int main() {\n"
            '    std::printf("cpp_keys ok\\n");\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(self.bake(["--trace", "run", str(root)]))

        self.assertIn("cpp_keys ok", output)
        self.assertIn("-DFROM_CXXFLAGS", output)
        self.assertIn("-std=c++17", output)

    def test_every_lang_key_is_accepted_without_a_suggestion(self) -> None:
        """Each documented lang key parses and is recognized.

        A key dropped from the parser's key table starts producing a
        'did you mean' warning, which this catches even for keys whose value
        bake does not act on yet.
        """
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"lang_all_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "lang_all_{stamp}",\n'
            '    "type": "application",\n'
            '    "lang.c": {\n'
            '        "cflags": [],\n'
            '        "cxxflags": [],\n'
            '        "defines": [],\n'
            '        "ldflags": [],\n'
            '        "lib": [],\n'
            '        "libpath": [],\n'
            '        "include": [],\n'
            '        "embed": [],\n'
            '        "c-standard": "c99",\n'
            '        "cpp-standard": "c++17"\n'
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text("int main(void) { return 0; }\n")

        output = self.strip_ansi(self.bake(["build", str(root)]))

        self.assertNotIn("did you mean", output)
        self.assertNotIn("unknown", output)

    def test_public_false_keeps_project_out_of_the_environment(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"private_project_{stamp}"
        public_id = f"public_pkg_{stamp}"
        hidden_id = f"hidden_pkg_{stamp}"

        for pkg_id, public in ((public_id, True), (hidden_id, False)):
            pkg = root / pkg_id
            (pkg / "src").mkdir(parents=True)
            (pkg / "project.json").write_text(
                f'{{"id": "{pkg_id}", "type": "package", '
                f'"value": {{"public": {"true" if public else "false"}}}}}\n'
            )
            (pkg / "src" / "a.c").write_text(f"int {pkg_id}_value(void) {{ return 1; }}\n")

        self.bake(["build", str(root)])

        state = self.list_state()
        self.assertIn(public_id, state.package_names)
        self.assertNotIn(
            hidden_id, state.package_names,
            '"public": false must keep a project out of the bake environment')

    def write_etc_package(self, name: str, public: bool = True) -> tuple[Path, str]:
        stamp = int(time.time() * 1_000_000)
        pkg_id = f"{name}_{stamp}"
        pkg_dir = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, pkg_dir, ignore_errors=True)
        (pkg_dir / "src").mkdir(parents=True)
        (pkg_dir / "etc" / "shaders").mkdir(parents=True)
        (pkg_dir / "project.json").write_text(
            "{\n"
            f'    "id": "{pkg_id}",\n'
            '    "type": "package",\n'
            f'    "value": {{"public": {"true" if public else "false"}}}\n'
            "}\n"
        )
        (pkg_dir / "src" / "a.c").write_text(f"int {name}_value(void) {{ return 1; }}\n")
        (pkg_dir / "etc" / "config.json").write_text('{"asset": 1}\n')
        (pkg_dir / "etc" / "shaders" / "main.wgsl").write_text("// shader\n")
        return pkg_dir, pkg_id

    def test_etc_folder_installs_on_build_of_public_project(self) -> None:
        pkg_dir, pkg_id = self.write_etc_package("etc_install")

        self.bake(["build", str(pkg_dir)])

        etc_root = self.bake_home / "etc" / pkg_id
        self.assertTrue(etc_root.is_dir(), f"Expected installed etc folder at {etc_root}")
        self.assertEqual((etc_root / "config.json").read_text(), '{"asset": 1}\n')
        self.assertEqual((etc_root / "shaders" / "main.wgsl").read_text(), "// shader\n")

    def test_etc_install_refreshes_changed_files_and_removes_stale_ones(self) -> None:
        pkg_dir, pkg_id = self.write_etc_package("etc_refresh")
        other_dir, other_id = self.write_etc_package("etc_other")

        self.bake(["build", str(pkg_dir)])
        self.bake(["build", str(other_dir)])

        (pkg_dir / "etc" / "config.json").write_text('{"asset": 2}\n')
        (pkg_dir / "etc" / "shaders" / "main.wgsl").unlink()
        (pkg_dir / "etc" / "extra.txt").write_text("extra\n")

        self.bake(["build", str(pkg_dir)])

        etc_root = self.bake_home / "etc" / pkg_id
        self.assertEqual(
            (etc_root / "config.json").read_text(), '{"asset": 2}\n',
            "A changed asset must be refreshed in the bake environment")
        self.assertFalse(
            (etc_root / "shaders" / "main.wgsl").exists(),
            "A removed asset must be removed from the bake environment")
        self.assertEqual((etc_root / "extra.txt").read_text(), "extra\n")

        other_root = self.bake_home / "etc" / other_id
        self.assertTrue(
            (other_root / "shaders" / "main.wgsl").is_file(),
            "Installing one project must not touch another project's etc folder")

        shutil.rmtree(pkg_dir / "etc")
        self.bake(["build", str(pkg_dir)])

        self.assertFalse(
            etc_root.exists(),
            "Removing the etc folder must remove the installed copy")
        self.assertTrue(other_root.is_dir(), f"Expected {other_root} to be kept")

    def test_etc_install_skips_private_projects(self) -> None:
        pkg_dir, pkg_id = self.write_etc_package("etc_private", public=False)

        self.bake(["build", str(pkg_dir)])

        self.assertFalse(
            (self.bake_home / "etc" / pkg_id).exists(),
            '"public": false must keep the etc folder out of the bake environment')

    def test_etc_installs_into_the_local_environment(self) -> None:
        pkg_dir, pkg_id = self.write_etc_package("etc_local_env")
        env_name = "etc_named_env"
        local_env_root = pkg_dir / ".bake" / "local_env"

        self.bake([f"--local-env={env_name}", "build", "."], cwd=pkg_dir)

        named_etc = local_env_root / env_name / "etc" / pkg_id
        self.assertEqual((named_etc / "config.json").read_text(), '{"asset": 1}\n')
        self.assertFalse(
            (self.bake_home / "etc" / pkg_id).exists(),
            "A local environment build must not install into the global environment")

        self.bake(["--local-env", "build", "."], cwd=pkg_dir)

        unnamed_etc = local_env_root / "etc" / pkg_id
        self.assertEqual((unnamed_etc / "config.json").read_text(), '{"asset": 1}\n')

    def test_run_process_receives_bake_home_and_environment(self) -> None:
        project_dir, _ = self.write_simple_app_project(
            "run_env_vars",
            "#include <stdio.h>\n"
            "#include <stdlib.h>\n"
            "int main(void) {\n"
            '    const char *home = getenv("BAKE_HOME");\n'
            '    const char *env = getenv("BAKE_ENVIRONMENT");\n'
            '    printf("BAKE_HOME=%s\\n", home ? home : "<unset>");\n'
            '    printf("BAKE_ENVIRONMENT=%s\\n", env ? env : "<unset>");\n'
            "    return 0;\n"
            "}\n",
        )
        env_name = "run_env_named"
        local_env_root = project_dir / ".bake" / "local_env"

        output = self.strip_ansi(self.bake(["run", "."], cwd=project_dir))
        self.assertIn(f"BAKE_HOME={self.bake_home}", output)
        self.assertIn("BAKE_ENVIRONMENT=global", output)

        output = self.strip_ansi(self.bake(["--local-env", "run", "."], cwd=project_dir))
        self.assertIn(f"BAKE_HOME={local_env_root}", output)
        self.assertIn("BAKE_ENVIRONMENT=local", output)

        output = self.strip_ansi(
            self.bake([f"--local-env={env_name}", "run", "."], cwd=project_dir))
        self.assertIn(f"BAKE_HOME={local_env_root / env_name}", output)
        self.assertIn(f"BAKE_ENVIRONMENT={env_name}", output)

    def test_info_prints_the_etc_install_path(self) -> None:
        pkg_dir, pkg_id = self.write_etc_package("etc_info")
        private_dir, _ = self.write_etc_package("etc_info_private", public=False)

        info = self.strip_ansi(self.bake(["info", "."], cwd=pkg_dir))
        expected = str(self.bake_home / "etc" / pkg_id)
        self.assertRegex(info, rf"(?m)^etc:\s+{re.escape(expected)}$")

        info = self.strip_ansi(self.bake(["info", "."], cwd=private_dir))
        self.assertRegex(info, r"(?m)^etc:\s+<none>$")

    def test_arch_conditional_applies_for_the_host_architecture(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"arch_cond_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "arch_cond_{stamp}",\n'
            '    "type": "application",\n'
            '    "lang.c": {\n'
            f'        "${{arch {self.host_arch()}}}": {{"defines": ["ARCH_MATCHED"]}},\n'
            '        "${arch nonexistent}": {"defines": ["ARCH_MISMATCHED"]}\n'
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text(
            "#ifndef ARCH_MATCHED\n"
            '#error "matching ${arch} conditional was not applied"\n'
            "#endif\n"
            "#ifdef ARCH_MISMATCHED\n"
            '#error "non-matching ${arch} conditional was applied"\n'
            "#endif\n"
            "int main(void) { return 0; }\n"
        )

        self.bake(["build", str(root)])

    def test_target_conditional_is_inert_without_a_target(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"target_cond_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "target_cond_{stamp}",\n'
            '    "type": "application",\n'
            '    "lang.c": {\n'
            '        "${target em}": {"defines": ["TARGET_EM"]}\n'
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text(
            "#ifdef TARGET_EM\n"
            '#error "${target em} was applied to a native build"\n'
            "#endif\n"
            "int main(void) { return 0; }\n"
        )

        self.bake(["build", str(root)])

    def test_build_integration_target(self) -> None:
        self.bake(["build", "test/integration"])
        state = self.list_state()
        self.assertIn("flecs", state.package_names)
        self.assertIn("city", state.application_names)
        self.assertIn("flecs.components.graphics", state.package_names)

    @unittest.skipIf(platform.system() != "Darwin", "flecs-engine ships a Cocoa-only native surface")
    def test_build_flecs_engine_target(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("cargo") is None:
            self.skipTest("cargo not available on PATH")
        self.bake(["build", "test/integration/flecs-engine"])
        state = self.list_state()
        self.assertIn("flecs_engine", state.application_names)

    def test_build_flecs_modules_target(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        state = self.list_state()
        self.assertIn("flecs", state.package_names)
        self.assertIn("flecs.components.transform", state.package_names)
        self.assertIn("city", state.application_names)

    def test_rebuild_and_incremental_from_repo_root(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        self.bake(["rebuild", "test/integration/flecs-modules-test/apps/city"])
        self.bake(["rebuild", "test/integration/flecs-modules-test/apps/tower_defense"])
        self.bake(["build", "test/integration/flecs-modules-test/apps/city"])
        self.bake(["build", "test/integration/flecs-modules-test/apps/tower_defense"])

    def test_rebuild_recursive_rebuilds_dependency_from_env_source(self) -> None:
        dep_target = "test/projects/envpkgs/libmath"
        app_dir = self.repo_root / "test" / "projects" / "ws" / "apps" / "use_env"

        self.bake(["build", dep_target])
        self.bake(["build", str(app_dir)])

        artefact = self.artefact_path(dep_target)
        initial_mtime = artefact.stat().st_mtime_ns

        self.bake(["rebuild"], cwd=app_dir)
        self.assertEqual(
            artefact.stat().st_mtime_ns,
            initial_mtime,
            "Expected non-recursive rebuild to leave the env dependency untouched",
        )

        time.sleep(0.02)
        output = self.strip_ansi(self.bake(["rebuild", "-r"], cwd=app_dir))
        self.assertIn("env.libs.math", output)
        self.assertTrue(artefact.exists(), f"Expected dependency artefact at {artefact}")
        self.assertGreater(
            artefact.stat().st_mtime_ns,
            initial_mtime,
            "Expected recursive rebuild to rebuild the dependency from its source location",
        )

    @unittest.skipIf(platform.system() == "Windows", "incremental rebuild check is flaky on Windows; see CI investigation")
    def test_tower_defense_incremental_does_not_rebuild_main_cpp(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        output = self.bake(["build", "test/integration/flecs-modules-test/apps/tower_defense"])
        self.assertNotIn("main.cpp", self.strip_ansi(output))

    def test_project_json_newer_than_artefact_triggers_rebuild(self) -> None:
        target = "test/projects/c/app_helloworld"
        project_json = self.repo_root / target / "project.json"

        self.bake(["build", target])
        artefact = self.artefact_path(target)
        objects_before = {obj: obj.stat().st_mtime_ns for obj in self.object_paths(target)}
        before_mtime = artefact.stat().st_mtime_ns

        time.sleep(0.02)
        os.utime(project_json, None)
        self.bake(["build", target])

        objects_after = {obj: obj.stat().st_mtime_ns for obj in self.object_paths(target)}
        after_mtime = artefact.stat().st_mtime_ns
        rebuilt_object = any(
            objects_after.get(obj, 0) > before
            for obj, before in objects_before.items()
        )

        self.assertTrue(
            rebuilt_object,
            "Expected at least one object file to be rebuilt after touching project.json",
        )
        self.assertGreater(
            after_mtime,
            before_mtime,
            "Expected build to update artefact after touching project.json",
        )

    def test_build_app_with_json_comments(self) -> None:
        target = "test/projects/c/app_w_comments"
        self.bake(["build", target])
        artefact = self.artefact_path(target)
        self.assertTrue(artefact.exists(), f"Expected artefact at {artefact}")
        state = self.list_state()
        self.assertIn("examples.c.app_w_comments", state.application_names)

    def test_build_app_with_non_c_source_extension_skips_inl(self) -> None:
        target = "test/projects/c/app_w_non_c_ext"
        self.bake(["build", target])
        objects = self.object_paths(target)
        names = [o.name for o in objects]
        self.assertTrue(
            any("main" in name for name in names),
            f"Expected main object to be compiled, got objects: {names}",
        )
        for name in names:
            self.assertFalse(
                "foo" in name and "inl" in name,
                f"Expected foo.inl to be skipped, but found compiled object: {name}",
            )

    def test_run_app_dependee_resolves_transitive_dependee_use(self) -> None:
        self.bake(["build", "test/projects/c/pkg_helloworld"])
        self.bake(["build", "test/projects/c/pkg_w_dependee"])
        self.bake(["build", "test/projects/c/app_dependee"])
        output = self.bake(["run", "test/projects/c/app_dependee"])
        text = self.strip_ansi(output)
        self.assertIn("pkg_w_dependee", text)
        self.assertIn("Hello world", text)

    def test_build_pkg_dependency_private_with_use_private(self) -> None:
        self.bake(["build", "test/projects/c/pkg_helloworld"])
        target = "test/projects/c/pkg_dependency_private"
        self.bake(["build", target])
        artefact = self.artefact_path(target)
        self.assertTrue(artefact.exists(), f"Expected artefact at {artefact}")

    def write_standalone_workspace(
        self, name: str, message: str = "Hello world"
    ) -> tuple[Path, str, str]:
        """Create a tmp workspace with a package and a standalone app using it.

        Returns (workspace dir, package id, app id). Project directories are
        <ws>/pkg and <ws>/app.
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"standalone_pkg_{stamp}"
        app_id = f"standalone_app_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package"}}\n'
        )
        guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {guard}\n"
            f"#define {guard}\n"
            "void pkg_print(void);\n"
            "#endif\n"
        )
        (pkg_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "void pkg_print(void) {\n"
            f'    printf("{message}\\n");\n'
            "}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "int main(void) {\n"
            "    pkg_print();\n"
            "    return 0;\n"
            "}\n"
        )

        return ws, pkg_id, app_id

    def test_standalone_flag_generates_amalgamated_deps(self) -> None:
        self.bake(["build", "test/projects/c/pkg_helloworld"])
        target = "test/projects/c/app_standalone"
        deps_dir = self.repo_root / target / "deps"
        self.addCleanup(shutil.rmtree, deps_dir, ignore_errors=True)
        if deps_dir.exists():
            shutil.rmtree(deps_dir)

        output = self.bake(["run", target])
        self.assertIn("Hello world", self.strip_ansi(output))

        header = deps_dir / "examples_c_pkg_helloworld.h"
        source = deps_dir / "examples_c_pkg_helloworld.c"
        marker = deps_dir / ".bake_standalone_deps"
        self.assertTrue(header.exists(), f"Expected amalgamated header at {header}")
        self.assertTrue(source.exists(), f"Expected amalgamated source at {source}")
        self.assertTrue(marker.exists(), f"Expected marker at {marker}")

        header_text = header.read_text()
        self.assertIn("#define examples_c_pkg_helloworld_STATIC", header_text)
        self.assertIn("void pkg_helloworld(void);", header_text)

        marker_text = marker.read_text()
        self.assertIn("examples.c.pkg_helloworld=", marker_text)

        obj_names = [p.name for p in self.object_paths(target)]
        self.assertTrue(
            any("examples_c_pkg_helloworld" in name for name in obj_names),
            f"Expected dependency object among {obj_names}",
        )

    def test_standalone_deps_refresh_when_dependency_changes(self) -> None:
        ws, pkg_id, _ = self.write_standalone_workspace("standalone_refresh")

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello world", self.strip_ansi(output))

        pkg_main = ws / "pkg" / "src" / "main.c"
        pkg_main.write_text(pkg_main.read_text().replace("Hello world", "Hello universe"))

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello universe", self.strip_ansi(output))

        deps_source = ws / "app" / "deps" / f"{pkg_id}.c"
        self.assertIn("Hello universe", deps_source.read_text())

    def test_standalone_rebuild_uses_existing_deps_when_dependency_missing(self) -> None:
        ws, pkg_id, _ = self.write_standalone_workspace("standalone_no_dep")

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello world", self.strip_ansi(output))

        shutil.rmtree(ws / "pkg")

        self.bake(["rebuild", "app"], cwd=ws)
        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello world", self.strip_ansi(output))

        deps_dir = ws / "app" / "deps"
        self.assertTrue((deps_dir / f"{pkg_id}.h").exists())
        self.assertTrue((deps_dir / f"{pkg_id}.c").exists())

    def test_standalone_build_fails_clearly_when_dependency_missing(self) -> None:
        ws, _, _ = self.write_standalone_workspace("standalone_missing")
        shutil.rmtree(ws / "pkg")

        output = self.bake_expect_failure(["build", "app"], cwd=ws)
        self.assertIn(
            "cannot generate standalone sources", self.strip_ansi(output)
        )

    @classmethod
    def _git_init_repo(cls, path: Path) -> None:
        git_env = {
            **cls.env,
            "GIT_AUTHOR_NAME": "bake",
            "GIT_AUTHOR_EMAIL": "bake@example.com",
            "GIT_COMMITTER_NAME": "bake",
            "GIT_COMMITTER_EMAIL": "bake@example.com",
        }
        for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "init"]):
            subprocess.run(["git", *cmd], cwd=path, check=True, env=git_env)

    @classmethod
    def git_commit_all(cls, path: Path, message: str) -> None:
        git_args = [
            "git",
            "-c", "commit.gpgsign=false",
            "-c", "user.email=bundle@test",
            "-c", "user.name=bundle test",
        ]
        cls.run_cmd([*git_args, "add", "-A"], cwd=path)
        cls.run_cmd([*git_args, "commit", "-q", "-m", message], cwd=path)

    @classmethod
    def git_tag(cls, path: Path, tag: str) -> None:
        cls.run_cmd(["git", "tag", tag], cwd=path)

    def write_bundle_consumer_workspace(self, name: str) -> tuple[Path, Path, Path]:
        """Create a package that re-exports a bundle header through its public
        header, plus a separate application that consumes the package.

        The bundle is a header-only local git repository. Because the app lives
        in its own directory tree, the package is resolved as an external
        (installed) dependency rather than discovered locally -- the same shape
        as an app using flecs_engine, whose public header includes
        <cglm/cglm.h> from a bundle. Returns (workspace, pkg dir, app dir).
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"bundle_pkg_{stamp}"
        app_id = f"bundle_app_{stamp}"
        bundle_id = f"bundlelib_{stamp}"

        # Header-only bundle source as a standalone local git repository. The
        # header sits under <bundle_id>/ so it is reachable as
        # <bundle_id>/<bundle_id>.h once the bundle's include path is added.
        repo = ws / "bundle_repo"
        (repo / bundle_id).mkdir(parents=True)
        bundle_guard = f"{bundle_id.upper()}_H"
        (repo / bundle_id / f"{bundle_id}.h").write_text(
            f"#ifndef {bundle_guard}\n"
            f"#define {bundle_guard}\n"
            "#define BUNDLE_VALUE 42\n"
            "#endif\n"
        )
        self._git_init_repo(repo)

        pkg_dir = ws / "pkg"
        app_dir = ws / "app"
        pkg_include = pkg_dir / "include"
        pkg_src = pkg_dir / "src"
        app_src = app_dir / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (pkg_dir / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package", '
            f'"bundle": {{"{bundle_id}": '
            f'{{"repository": "{repo.as_posix()}", "header-only": true}}}}}}\n'
        )
        pkg_guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {pkg_guard}\n"
            f"#define {pkg_guard}\n"
            f"#include <{bundle_id}/{bundle_id}.h>\n"
            "int pkg_value(void);\n"
            "#endif\n"
        )
        (pkg_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "int pkg_value(void) {\n"
            "    return BUNDLE_VALUE;\n"
            "}\n"
        )

        (app_dir / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"]}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("value=%d\\n", pkg_value());\n'
            "    return 0;\n"
            "}\n"
        )

        return ws, pkg_dir, app_dir

    def test_external_dependency_bundle_include_propagates_to_consumer(self) -> None:
        _, pkg_dir, app_dir = self.write_bundle_consumer_workspace("bundle_consumer")

        # Build and install the package; this prepares (clones) its bundle.
        self.bake(["build"], cwd=pkg_dir)

        # The app pulls in the package's public header, which includes the
        # bundle header. The package is resolved from the environment, so its
        # bundle include path must propagate to the consumer for this to build.
        output = self.bake(["run"], cwd=app_dir)
        self.assertIn("value=42", self.strip_ansi(output))

    def write_mixed_language_standalone_workspace(
        self, name: str
    ) -> tuple[Path, str, str]:
        """Workspace with a C++ package that also contains C sources, consumed
        by a standalone app. The C source uses a C-only idiom (implicit void*
        conversion) that fails when compiled as C++, so amalgamating it into a
        single .cpp would break the build -- it must land in its own .c file.
        Mirrors a C++ package like flecs_engine whose implementation is mostly
        C. Returns (workspace, package id, app id).
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"mixed_pkg_{stamp}"
        app_id = f"mixed_app_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package", '
            f'"value": {{"language": "c++"}}}}\n'
        )
        guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {guard}\n"
            f"#define {guard}\n"
            "#ifdef __cplusplus\n"
            'extern "C" {\n'
            "#endif\n"
            "int c_part_value(void);\n"
            "int cpp_part_value(void);\n"
            "#ifdef __cplusplus\n"
            "}\n"
            "#endif\n"
            "#endif\n"
        )
        # C source: the implicit void*->int* conversion is valid C but an error
        # in C++, so this only compiles when emitted to a .c file.
        (pkg_src / "c_part.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "int c_part_value(void) {\n"
            "    void *v = 0;\n"
            "    int *p = v;\n"
            "    (void)p;\n"
            "    return 21;\n"
            "}\n"
        )
        # C++ source: a template makes this valid C++ but not C.
        (pkg_src / "cpp_part.cpp").write_text(
            f"#include <{pkg_id}.h>\n"
            "template<typename T> static T add21(T a) { return a + T(21); }\n"
            "int cpp_part_value(void) {\n"
            "    return add21<int>(0);\n"
            "}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("sum=%d\\n", c_part_value() + cpp_part_value());\n'
            "    return 0;\n"
            "}\n"
        )

        return ws, pkg_id, app_id

    def test_standalone_amalgamation_splits_c_and_cpp_sources(self) -> None:
        ws, pkg_id, _ = self.write_mixed_language_standalone_workspace(
            "mixed_lang")

        # Without the C/C++ split the package's C source would be compiled as
        # C++ and fail; the app builds and runs only when they are separated.
        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("sum=42", self.strip_ansi(output))

        deps_dir = ws / "app" / "deps"
        self.assertTrue(
            (deps_dir / f"{pkg_id}.c").exists(),
            f"Expected C amalgamation at {deps_dir / f'{pkg_id}.c'}")
        self.assertTrue(
            (deps_dir / f"{pkg_id}.cpp").exists(),
            f"Expected C++ amalgamation at {deps_dir / f'{pkg_id}.cpp'}")

        # The C source's body must live in the .c file, not the .cpp file.
        self.assertIn("c_part_value", (deps_dir / f"{pkg_id}.c").read_text())
        self.assertIn("cpp_part_value", (deps_dir / f"{pkg_id}.cpp").read_text())

    def test_standalone_amalgamation_removes_stale_language_output(self) -> None:
        """A C-only dependency amalgamates to <base>.c only. A leftover
        <base>.cpp from an older bake (which duplicated the C body into a .cpp)
        must be removed on regeneration, otherwise it is compiled as C++ and
        breaks the build.
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"stale_cpp_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"stale_pkg_{stamp}"
        app_id = f"stale_app_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package"}}\n'
        )
        guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {guard}\n#define {guard}\n"
            "int dep_value(void);\n#endif\n"
        )
        # C-only idiom that fails when compiled as C++, so a stale .cpp copy of
        # this body would not build.
        (pkg_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "int dep_value(void) {\n"
            "    void *v = 0;\n"
            "    int *p = v;\n"
            "    (void)p;\n"
            "    return 42;\n"
            "}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("value=%d\\n", dep_value());\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("value=42", self.strip_ansi(output))

        deps_dir = ws / "app" / "deps"
        c_out = deps_dir / f"{pkg_id}.c"
        cpp_out = deps_dir / f"{pkg_id}.cpp"
        self.assertTrue(c_out.exists())
        self.assertFalse(cpp_out.exists())

        # Simulate the leftover .cpp an older bake produced, then force the
        # dependency to regenerate by changing its source.
        cpp_out.write_text(c_out.read_text())
        (pkg_src / "main.c").write_text(
            (pkg_src / "main.c").read_text().replace("return 42;", "return 43;")
        )

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("value=43", self.strip_ansi(output))
        self.assertFalse(
            cpp_out.exists(),
            f"Stale C++ amalgamation was not removed: {cpp_out}")

    def test_standalone_propagates_dependency_defines(self) -> None:
        """A standalone app compiles its dependency's amalgamated source into
        its own binary. Defines the dependency declares in its project.json
        (like flecs_engine's GLFW_EXPOSE_NATIVE_COCOA) must reach that source,
        even though it is built with the app's compile flags.
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"define_dep_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"define_pkg_{stamp}"
        app_id = f"define_app_{stamp}"
        flag = f"NEED_FLAG_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package", '
            f'"lang.c": {{"defines": ["{flag}"]}}}}\n'
        )
        guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {guard}\n#define {guard}\n"
            "int dep_value(void);\n#endif\n"
        )
        # Only compiles when the package's own define is in effect.
        (pkg_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            f"#ifndef {flag}\n"
            f'#error "{flag} not defined"\n'
            "#endif\n"
            "int dep_value(void) {\n"
            "    return 42;\n"
            "}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("value=%d\\n", dep_value());\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("value=42", self.strip_ansi(output))

    def test_standalone_amalgamation_dedups_unconditional_includes(self) -> None:
        """Multiple source files in an amalgamated unit that each include the
        same external header would otherwise repeat it -- harmless for guarded
        headers but fatal for single-header libraries whose implementation macro
        leaks across the merged unit (e.g. stb_image.h via STB_IMAGE_IMPLEMENTATION).
        A top-level repeat is dropped; a repeat guarded by #if is preserved,
        since the guard may be selecting between alternatives.
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"dedup_inc_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"dedup_pkg_{stamp}"
        app_id = f"dedup_app_{stamp}"
        guard = f"DEDUP_GUARD_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package"}}\n'
        )
        hguard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {hguard}\n#define {hguard}\n"
            "int a_val(void);\nint b_val(void);\n#endif\n"
        )
        (pkg_src / "a.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int a_val(void) {\n    return 1;\n}\n"
        )
        # Second top-level <stdio.h> is a duplicate (dropped); the one guarded by
        # #ifdef is kept.
        (pkg_src / "b.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            f"#ifdef {guard}\n"
            "#include <stdio.h>\n"
            "#endif\n"
            "int b_val(void) {\n    return 2;\n}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("sum=%d\\n", a_val() + b_val());\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("sum=3", self.strip_ansi(output))

        amalg = (ws / "app" / "deps" / f"{pkg_id}.c").read_text()
        # One top-level include survives plus the #ifdef-guarded one: two total.
        self.assertEqual(
            amalg.count("#include <stdio.h>"), 2,
            f"expected the top-level duplicate dropped and the guarded one kept:\n{amalg}")
        self.assertIn(f"#ifdef {guard}", amalg)

    def test_header_mtime_triggers_rebuild(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir = self.repo_root / "test" / "tmp" / f"header_mtime_{stamp}"
        src_dir = project_dir / "src"
        include_dir = project_dir / "include"
        src_dir.mkdir(parents=True, exist_ok=True)
        include_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.header_mtime.{stamp}\",\n"
            "    \"type\": \"application\"\n"
            "}\n"
        )
        header = include_dir / "myheader.h"
        header.write_text(
            "#ifndef MYHEADER_H\n"
            "#define MYHEADER_H\n"
            "static int header_value(void) { return 1; }\n"
            "#endif\n"
        )
        (src_dir / "main.c").write_text(
            "#include \"myheader.h\"\n"
            "int main(void) {\n"
            "    return header_value() - 1;\n"
            "}\n"
        )

        self.bake(["build", str(project_dir)])
        objects_before = {
            obj: obj.stat().st_mtime_ns for obj in self.object_paths(str(project_dir.relative_to(self.repo_root)))
        }
        self.assertGreater(len(objects_before), 0, "Expected at least one object file")

        time.sleep(1.1)
        os.utime(header, None)

        self.bake(["build", str(project_dir)])
        objects_after = {
            obj: obj.stat().st_mtime_ns for obj in self.object_paths(str(project_dir.relative_to(self.repo_root)))
        }
        rebuilt = any(
            objects_after.get(obj, 0) > before
            for obj, before in objects_before.items()
        )
        self.assertTrue(
            rebuilt,
            "Expected object file to be rebuilt after touching included header",
        )

    def test_rebuild_from_test_directory(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        test_dir = self.repo_root / "test"
        self.bake(["rebuild", "integration/flecs-modules-test/apps/city"], cwd=test_dir)
        self.bake(
            ["rebuild", "integration/flecs-modules-test/apps/tower_defense"], cwd=test_dir
        )

    def test_rebuild_from_integration_directory(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        integration_dir = self.repo_root / "test" / "integration"
        self.bake(["rebuild", "flecs-modules-test/apps/city"], cwd=integration_dir)
        self.bake(
            ["rebuild", "flecs-modules-test/apps/tower_defense"], cwd=integration_dir
        )
        state = self.list_state(cwd=integration_dir)
        self.assertIn("flecs", state.package_names)
        self.assertIn("city", state.application_names)
        self.assertIn("tower_defense", state.application_names)

    def test_run_query_from_root(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test/flecs"])
        output = self.bake(
            ["run", "test/integration/flecs-modules-test/flecs/test/query", "--", "-j", "12"]
        )
        self.assertIn("PASS:", self.strip_ansi(output))

    def test_build_from_flecs_directory_with_no_target(self) -> None:
        flecs_dir = self.repo_root / "test" / "integration" / "flecs-modules-test" / "flecs"
        self.bake([], cwd=flecs_dir)

        state = self.list_state(cwd=flecs_dir)
        self.assertIn("flecs", state.package_names)
        self.assertEqual(state.application_names, frozenset())
        self.assertEqual(state.package_names - {"flecs"}, frozenset())

    def test_run_query_from_flecs_directory_and_empty_env(self) -> None:
        flecs_dir = self.repo_root / "test" / "integration" / "flecs-modules-test" / "flecs"
        self.bake(["run", "test/query", "--", "-j", "12"], cwd=flecs_dir)
        self.bake(["clean"], cwd=flecs_dir)
        self.bake(["reset"], cwd=flecs_dir)
        self.bake(["run", "test/query", "--", "-j", "12"], cwd=flecs_dir)
        self.bake(["run", "test/query", "--", "-j", "12"], cwd=flecs_dir)

        state = self.list_state(cwd=flecs_dir)
        self.assertIn("flecs", state.package_names)
        self.assertEqual(state.application_names, frozenset())
        self.assertEqual(state.package_names - {"flecs"}, frozenset())

    def test_test_command_builds_and_runs_test_project(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.testcmd.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"test_cmd_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"test_cmd_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )
        (src_dir / "Math.c").write_text("void Math_add(void) { }\n")

        output = self.strip_ansi(self.bake(["test", str(project_dir)]))
        self.assertIn("PASS", output)

    def test_project_json_testsuites_generate_missing_stubs(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        math_c = src_dir / "Math.c"
        initial_math = (
            "int helper_value  (void) {\n"
            "    return 42;\n"
            "}\n"
            "\n"
            "void Math_add(void){ }\n"
        )
        math_c.write_text(initial_math)

        self.bake(["build", str(project_dir)])

        time.sleep(1.1)
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\", \"sub\"]\n"
            "        }, {\n"
            "            \"id\": \"Util\",\n"
            "            \"teardown\": true,\n"
            "            \"testcases\": [\"case_1\"],\n"
            "            \"params\": {\n"
            "                \"mode\": [\"fast\", \"slow\"]\n"
            "            }\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        self.bake(["build", str(project_dir)])

        math_text = math_c.read_text()
        self.assertTrue(
            math_text.startswith(initial_math),
            "Existing code in suite source file changed unexpectedly",
        )
        self.assertEqual(math_text.count("void Math_add(void)"), 1)
        self.assertIn("void Math_sub(void) {\n}\n", math_text)
        self.assertIn("int helper_value  (void)", math_text)

        util_c = src_dir / "Util.c"
        self.assertTrue(util_c.exists(), "Expected missing suite source file to be created")
        util_text = util_c.read_text()
        self.assertIn("void Util_teardown(void) {\n}\n", util_text)
        self.assertIn("void Util_case_1(void) {\n}\n", util_text)

        main_c = src_dir / "main.c"
        self.assertTrue(main_c.exists(), "Expected generated src/main.c file")
        main_text = main_c.read_text()
        self.assertEqual(
            main_text,
            (
                "\n"
                "/* A friendly warning from bake.test\n"
                " * ----------------------------------------------------------------------------\n"
                " * This file is generated. To add/remove testcases modify the 'project.json' of\n"
                " * the test project. ANY CHANGE TO THIS FILE IS LOST AFTER (RE)BUILDING!\n"
                " * ----------------------------------------------------------------------------\n"
                " */\n"
                "\n"
                "#include <bake_test.h>\n"
                "\n"
                "// Testsuite 'Math'\n"
                "void Math_add(void);\n"
                "void Math_sub(void);\n"
                "\n"
                "// Testsuite 'Util'\n"
                "void Util_teardown(void);\n"
                "void Util_case_1(void);\n"
                "\n"
                "bake_test_case Math_testcases[] = {\n"
                "    {\n"
                "        \"add\",\n"
                "        Math_add\n"
                "    },\n"
                "    {\n"
                "        \"sub\",\n"
                "        Math_sub\n"
                "    }\n"
                "};\n"
                "\n"
                "bake_test_case Util_testcases[] = {\n"
                "    {\n"
                "        \"case_1\",\n"
                "        Util_case_1\n"
                "    }\n"
                "};\n"
                "\n"
                "const char* Util_mode_param[] = {\"fast\", \"slow\"};\n"
                "bake_test_param Util_params[] = {\n"
                "    {\"mode\", (char**)Util_mode_param, 2}\n"
                "};\n"
                "\n"
                "static bake_test_suite suites[] = {\n"
                "    {\n"
                "        \"Math\",\n"
                "        NULL,\n"
                "        NULL,\n"
                "        2,\n"
                "        Math_testcases\n"
                "    },\n"
                "    {\n"
                "        \"Util\",\n"
                "        NULL,\n"
                "        Util_teardown,\n"
                "        1,\n"
                "        Util_testcases,\n"
                "        1,\n"
                "        Util_params\n"
                "    }\n"
                "};\n"
                "\n"
                "int main(int argc, char *argv[]) {\n"
                f"    return bake_test_run(\"{project_id}\", argc, argv, suites, 2);\n"
                "}\n"
            ),
        )

    def test_project_json_testsuites_generate_stub_when_only_commented_definition(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.comment.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_comment_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_comment\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        math_c = src_dir / "Math.c"
        math_c.write_text(
            "// void Math_add(void) { /* commented out */ }\n"
            "/* void Math_add(void) {\n"
            "    return;\n"
            "} */\n"
        )

        self.bake(["build", str(project_dir)])

        math_text = math_c.read_text()
        self.assertIn("void Math_add(void) {\n}\n", math_text)

    def test_project_json_testsuites_main_uses_project_header_when_available(self) -> None:
        stamp = int(time.time() * 1_000_000)
        header_name = f"header_{stamp}"
        project_id = f"compat.{header_name}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_header_{stamp}"
        src_dir = project_dir / "src"
        include_dir = project_dir / "include"
        src_dir.mkdir(parents=True, exist_ok=True)
        include_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_header\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        header_guard = f"{header_name.upper()}_H"
        (include_dir / f"{header_name}.h").write_text(
            f"#ifndef {header_guard}\n"
            f"#define {header_guard}\n\n"
            f"#include <compat-{header_name}/bake_config.h>\n\n"
            "#endif\n"
        )

        self.bake(["build", str(project_dir)])

        main_c = src_dir / "main.c"
        self.assertTrue(main_c.exists(), "Expected generated src/main.c file")
        main_text = main_c.read_text()
        self.assertIn(f"#include <{header_name}.h>", main_text)
        self.assertNotIn("#include <bake_test.h>", main_text)

    def test_project_json_testsuites_do_not_rewrite_unchanged_files(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.mtime.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_mtime_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json_content = (
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_mtime\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\", \"sub\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )
        project_json.write_text(project_json_content)

        self.bake(["build", str(project_dir)])

        math_c = src_dir / "Math.c"
        main_c = src_dir / "main.c"
        self.assertTrue(math_c.exists())
        self.assertTrue(main_c.exists())

        math_before = math_c.stat().st_mtime_ns
        main_before = main_c.stat().st_mtime_ns

        self.bake(["build", str(project_dir)])
        self.assertEqual(math_before, math_c.stat().st_mtime_ns)
        self.assertEqual(main_before, main_c.stat().st_mtime_ns)

        time.sleep(1.1)
        project_json.write_text(project_json_content)
        self.bake(["build", str(project_dir)])

        self.assertEqual(
            math_before,
            math_c.stat().st_mtime_ns,
            "Suite source was rewritten even though generated content was unchanged",
        )
        self.assertEqual(
            main_before,
            main_c.stat().st_mtime_ns,
            "main.c was rewritten even though generated content was unchanged",
        )

    def test_project_json_testsuites_report_empty_testcases(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.empty.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_empty_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_empty\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        self.bake(["build", str(project_dir)])

        output = self.strip_ansi(self.bake(["run", str(project_dir)]))

        self.assertIn("EMPTY Math.add (add test statements)", output)
        self.assertIn("PASS:  0, FAIL:  0, EMPTY:  1", output)
        self.assertNotIn("PASS:  1, FAIL:  0, EMPTY:  0", output)

    def test_test_harness_parallelizes_cases_and_preserves_summaries(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.parallel.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_parallel_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        case_names = [f"case_{i}" for i in range(1, 7)]
        (project_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "test",
                    "test": {
                        "testsuites": [
                            {"id": "Alpha", "testcases": case_names},
                            {"id": "Beta", "testcases": case_names},
                        ]
                    },
                },
                indent=4,
            ) + "\n"
        )

        for suite in ("Alpha", "Beta"):
            functions = "".join(
                f'void {suite}_{case}(void) {{ wait_case("{suite}.{case}"); }}\n'
                for case in case_names
            )
            (src_dir / f"{suite}.c").write_text(
                "#include <bake_test.h>\n"
                "#include <stdio.h>\n"
                "#if defined(_WIN32)\n"
                "#include <windows.h>\n"
                "#define runner_sleep() Sleep(1000)\n"
                "#else\n"
                "#include <unistd.h>\n"
                "#define runner_sleep() sleep(1)\n"
                "#endif\n"
                "static void wait_case(const char *id) { runner_sleep(); printf(\"CASE %s\\n\", id); test_assert(true); }\n"
                f"{functions}"
            )

        local_env = "--local-env=parallel_cases"
        self.bake([local_env, "build", "."], cwd=project_dir)

        start = time.monotonic()
        serial_output = self.bake(
            [local_env, "run", ".", "--", "-j", "1"], cwd=project_dir)
        serial_elapsed = time.monotonic() - start

        start = time.monotonic()
        parallel_output = self.bake(
            [local_env, "run", ".", "--", "-j", "6"], cwd=project_dir)
        parallel_elapsed = time.monotonic() - start

        def summaries(output: str) -> str:
            return "".join(
                line for line in output.splitlines(keepends=True)
                if line.startswith("PASS:") and f"({project_id}." in line
            )

        serial_summaries = summaries(serial_output)
        parallel_summaries = summaries(parallel_output)
        expected_summaries = (
            f"PASS:  6, FAIL:  0, EMPTY:  0 ({project_id}.Alpha)\n"
            f"PASS:  6, FAIL:  0, EMPTY:  0 ({project_id}.Beta)\n"
            f"PASS: 12, FAIL:  0, EMPTY:  0 ({project_id}.all)\n"
        )

        self.assertEqual(serial_summaries, expected_summaries)
        self.assertEqual(parallel_summaries, serial_summaries)
        expected_cases = [
            f"CASE {suite}.{case}"
            for suite in ("Alpha", "Beta")
            for case in case_names
        ]
        serial_cases = [
            line for line in serial_output.splitlines() if line.startswith("CASE ")
        ]
        parallel_cases = [
            line for line in parallel_output.splitlines() if line.startswith("CASE ")
        ]
        self.assertEqual(serial_cases, expected_cases)
        self.assertEqual(parallel_cases, serial_cases)
        self.assertGreater(serial_elapsed, 10.0)
        self.assertLess(
            parallel_elapsed,
            serial_elapsed * 0.4,
            f"serial={serial_elapsed:.3f}s parallel={parallel_elapsed:.3f}s",
        )

    def test_test_harness_writes_json_report(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.json.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_json_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "test",
                    "test": {
                        "testsuites": [
                            {"id": "Alpha", "testcases": ["ok", "bad", "none"]},
                        ]
                    },
                },
                indent=4,
            ) + "\n"
        )
        (src_dir / "Alpha.c").write_text(
            "#include <bake_test.h>\n"
            "void Alpha_ok(void) { test_assert(true); }\n"
            "void Alpha_bad(void) { test_assert(false); }\n"
            "void Alpha_none(void) { }\n"
        )

        local_env = "--local-env=json_report"
        self.bake([local_env, "build", "."], cwd=project_dir)

        for jobs in ("1", "3"):
            report = project_dir / f"report_{jobs}.json"
            self.bake_expect_failure(
                [local_env, "run", ".", "--", "-j", jobs, "--json", str(report)],
                cwd=project_dir)
            self.assertTrue(report.is_file(), f"missing report {report}")
            data = json.loads(report.read_text())
            self.assertEqual(data["project"], project_id)
            self.assertEqual(data["pass"], 1)
            self.assertEqual(data["fail"], 1)
            self.assertEqual(data["empty"], 1)
            self.assertGreater(data["elapsed"], 0.0)
            by_case = {t["case"]: t for t in data["tests"]}
            self.assertEqual(set(by_case), {"ok", "bad", "none"})
            self.assertEqual(by_case["ok"]["status"], "pass")
            self.assertEqual(by_case["bad"]["status"], "fail")
            self.assertEqual(by_case["none"]["status"], "empty")
            for t in data["tests"]:
                self.assertEqual(t["suite"], "Alpha")
                self.assertGreater(t["elapsed"], 0.0)

    def test_test_harness_writes_coverage_report(self) -> None:
        if platform.system() == "Windows":
            self.skipTest("coverage is not supported on Windows")
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("no C compiler available")
        version = subprocess.run(
            [compiler, "--version"], text=True, capture_output=True, check=False)
        if "clang" not in (version.stdout or ""):
            self.skipTest("coverage requires clang")

        stamp = int(time.time() * 1_000_000)
        lib_id = f"tmp.tests.coverage.lib.{stamp}"
        test_id = f"tmp.tests.coverage.test.{stamp}"
        root = self.repo_root / "test" / "tmp" / f"harness_coverage_{stamp}"
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        lib_dir = root / "lib"
        test_dir = root / "test"
        (lib_dir / "src").mkdir(parents=True, exist_ok=True)
        (lib_dir / "include").mkdir(parents=True, exist_ok=True)
        (test_dir / "src").mkdir(parents=True, exist_ok=True)

        (lib_dir / "project.json").write_text(
            json.dumps({"id": lib_id, "type": "package"}, indent=4) + "\n")
        (lib_dir / "include" / "covlib.h").write_text(
            "int covlib_sign(int x);\n"
            "int covlib_unused(void);\n"
        )
        (lib_dir / "src" / "covlib.c").write_text(
            "#include <covlib.h>\n"
            "int covlib_sign(int x) {\n"
            "    if (x > 0) {\n"
            "        return 1;\n"
            "    }\n"
            "    return -1;\n"
            "}\n"
            "int covlib_unused(void) {\n"
            "    return 42;\n"
            "}\n"
        )
        (test_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": test_id,
                    "type": "test",
                    "value": {"use": [lib_id]},
                    "test": {
                        "testsuites": [
                            {"id": "Sign", "testcases": ["pos", "neg"]},
                        ]
                    },
                },
                indent=4,
            ) + "\n"
        )
        (test_dir / "src" / "Sign.c").write_text(
            "#include <bake_test.h>\n"
            "#include <covlib.h>\n"
            "void Sign_pos(void) { test_int(covlib_sign(3), 1); }\n"
            "void Sign_neg(void) { test_int(covlib_sign(-3), -1); }\n"
        )

        local_env = "--local-env=coverage"
        report = root / "report.json"
        coverage = root / "report.coverage.json"
        for jobs in ("1", "2"):
            output = self.bake(
                [local_env, "run", "test", "--coverage", "--",
                 "-j", jobs, "--json", str(report)],
                cwd=root)
            self.assertIn("coverage: ", output)
            self.assertTrue(coverage.is_file(), f"missing report {coverage}")
            data = json.loads(coverage.read_text())
            self.assertEqual(data["project"], test_id)
            files = {Path(f["file"]).name: f for f in data["files"]}
            self.assertEqual(set(files), {"covlib.c"})
            lib = files["covlib.c"]
            self.assertEqual(lib["functions"]["count"], 2)
            self.assertEqual(lib["functions"]["covered"], 1)
            self.assertEqual(
                [f["name"] for f in lib["uncovered_functions"]],
                ["covlib_unused"])
            self.assertEqual(lib["uncovered_lines"], [[8, 10]])
            self.assertEqual(data["lines"], lib["lines"])
            self.assertEqual(data["lines"]["covered"], 6)

        self.assertEqual(list(test_dir.glob("*.profraw")), [])
        env_root = root / ".bake" / "local_env" / "coverage" / "build"
        self.assertTrue(list(env_root.glob("**/coverage/*.profraw")))

        output = self.bake([local_env, "coverage-report"], cwd=root)
        self.assertIn(f"coverage of 1 test project: {test_id}", output)
        self.assertRegex(output, r"lib/src\s+66\.67%\s+6/9")
        self.assertRegex(output, r"total\s+66\.67%\s+6/9\s+50\.00%\s+1/2")
        report_dir = root / ".bake" / "local_env" / "coverage" / "coverage_report"
        self.assertIn(str(report_dir / "index.html"), output)
        index = (report_dir / "index.html").read_text()
        self.assertNotIn("/*BAKE_COVERAGE_DATA*/", index)
        self.assertIn('"files": [\n["lib/src/covlib.c",[6,9],[1,2],', index)
        file_data = (report_dir / "files" / "0.js").read_text()
        self.assertTrue(file_data.startswith("bakeCoverageFile(0, {"))
        self.assertIn('"covlib_unused",8,0', file_data)
        self.assertIn("[9,0]", file_data)
        combined = json.loads((report_dir / "coverage.json").read_text())
        self.assertEqual(combined["projects"], [test_id])
        self.assertEqual(combined["lines"], data["lines"])
        self.assertEqual(combined["files"][0]["uncovered_lines"], [[8, 10]])

        self.bake([local_env, "clean", "test"], cwd=root)
        self.assertEqual(list(env_root.glob("**/coverage/*")), [])
        self.assertFalse(report_dir.exists())

        output = self.bake_expect_failure([local_env, "coverage-report"], cwd=root)
        self.assertIn("no coverage data found", output)

        coverage.unlink()
        self.bake([local_env, "run", "test", "--", "--json", str(report)],
            cwd=root)
        self.assertFalse(coverage.exists())

    def require_coverage_compiler(self) -> None:
        if platform.system() == "Windows":
            self.skipTest("coverage is not supported on Windows")
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("no C compiler available")
        version = subprocess.run(
            [compiler, "--version"], text=True, capture_output=True, check=False)
        if "clang" not in (version.stdout or ""):
            self.skipTest("coverage requires clang")

    def write_coverage_workspace(self, name: str) -> tuple[Path, str]:
        """Write a library with sources in src, src/util and include, and a test."""
        stamp = int(time.time() * 1_000_000)
        lib_id = f"tmp.tests.{name}.lib.{stamp}"
        test_id = f"tmp.tests.{name}.test.{stamp}"
        root = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        lib_dir = root / "lib"
        test_dir = root / "test"
        (lib_dir / "src" / "util").mkdir(parents=True, exist_ok=True)
        (lib_dir / "include").mkdir(parents=True, exist_ok=True)
        (test_dir / "src").mkdir(parents=True, exist_ok=True)

        (lib_dir / "project.json").write_text(
            json.dumps({"id": lib_id, "type": "package"}, indent=4) + "\n")
        (lib_dir / "include" / "covlib.h").write_text(
            "int covlib_sign(int x);\n"
            "int covlib_unused(void);\n"
            "int covlib_extra(int x);\n"
            "static inline int covlib_twice(int x) {\n"
            "    return x * 2;\n"
            "}\n"
        )
        (lib_dir / "src" / "covlib.c").write_text(
            "#include <covlib.h>\n"
            "int covlib_sign(int x) {\n"
            "    if (x > 0) {\n"
            "        return 1;\n"
            "    }\n"
            "    return -1;\n"
            "}\n"
            "int covlib_unused(void) {\n"
            "    return 42;\n"
            "}\n"
        )
        (lib_dir / "src" / "util" / "extra.c").write_text(
            "#include <covlib.h>\n"
            "int covlib_extra(int x) {\n"
            "    return x + 1;\n"
            "}\n"
        )
        (test_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": test_id,
                    "type": "test",
                    "value": {"use": [lib_id]},
                    "test": {
                        "testsuites": [
                            {"id": "Sign", "testcases": ["pos", "neg"]},
                        ]
                    },
                },
                indent=4,
            ) + "\n"
        )
        (test_dir / "src" / "Sign.c").write_text(
            "#include <bake_test.h>\n"
            "#include <covlib.h>\n"
            "void Sign_pos(void) { test_int(covlib_sign(3), 1); }\n"
            "void Sign_neg(void) {\n"
            "    test_int(covlib_sign(-3), -1);\n"
            "    test_int(covlib_twice(covlib_extra(1)), 4);\n"
            "}\n"
        )
        return root, test_id

    def test_coverage_report_paths_are_relative_to_the_root(self) -> None:
        self.require_coverage_compiler()
        root, test_id = self.write_coverage_workspace("coverage_root")
        local_env = "--local-env=coverage"
        report = root / "report.json"
        coverage = root / "report.coverage.json"

        self.bake(
            [local_env, "run", "test", "--coverage", "--", "--json", str(report)],
            cwd=root)
        data = json.loads(coverage.read_text())
        self.assertEqual(data["project"], test_id)
        self.assertNotIn("projects", data)
        self.assertEqual(
            [f["file"] for f in data["files"]],
            ["lib/include/covlib.h", "lib/src/covlib.c", "lib/src/util/extra.c"])
        for key in ("lines", "functions", "branches"):
            for field in ("count", "covered"):
                self.assertEqual(
                    data[key][field],
                    sum(f[key][field] for f in data["files"]))

        self.bake(
            [local_env, "run", "test", "--coverage", "--coverage-root", "lib",
             "--", "--json", str(report)],
            cwd=root)
        data = json.loads(coverage.read_text())
        self.assertEqual(
            [f["file"] for f in data["files"]],
            ["include/covlib.h", "src/covlib.c", "src/util/extra.c"])

        self.bake(
            [local_env, "run", "test", "--coverage", "--coverage-root",
             str(root / "lib" / "src"), "--", "--json", str(report)],
            cwd=root)
        data = json.loads(coverage.read_text())
        files = [f["file"] for f in data["files"]]
        self.assertEqual(files[1:], ["covlib.c", "util/extra.c"])
        self.assertTrue(Path(files[0]).is_absolute())
        self.assertTrue(files[0].endswith("/lib/include/covlib.h"))

        output = self.strip_ansi(self.bake_expect_failure(
            [local_env, "run", "test", "--coverage", "--coverage-root", "missing",
             "--", "--json", str(report)],
            cwd=root))
        self.assertIn("coverage root", output)
        self.assertIn("is not a directory", output)

    def test_coverage_report_filters_files_by_prefix_and_glob(self) -> None:
        self.require_coverage_compiler()
        root, test_id = self.write_coverage_workspace("coverage_filter")
        local_env = "--local-env=coverage"
        report = root / "report.json"
        coverage = root / "report.coverage.json"

        def run(*options: str) -> dict:
            self.bake(
                [local_env, "run", "test", "--coverage", *options,
                 "--", "--json", str(report)],
                cwd=root)
            return json.loads(coverage.read_text())

        full = run()
        by_file = {f["file"]: f for f in full["files"]}

        data = run("--coverage-include", "lib/src")
        self.assertEqual(
            [f["file"] for f in data["files"]],
            ["lib/src/covlib.c", "lib/src/util/extra.c"])
        for key in ("lines", "functions", "branches"):
            for field in ("count", "covered"):
                self.assertEqual(
                    data[key][field],
                    by_file["lib/src/covlib.c"][key][field] +
                    by_file["lib/src/util/extra.c"][key][field])
        self.assertEqual(data["lines"]["count"], 12)
        self.assertEqual(data["lines"]["covered"], 9)
        self.assertEqual(data["lines"]["percent"], 75.0)

        data = run("--coverage-root", "lib", "--coverage-include", "src/,include",
                   "--coverage-exclude", "src/util")
        self.assertEqual(
            [f["file"] for f in data["files"]],
            ["include/covlib.h", "src/covlib.c"])

        data = run("--coverage-include", "**/*.h,lib/src/*/*.c")
        self.assertEqual(
            [f["file"] for f in data["files"]],
            ["lib/include/covlib.h", "lib/src/util/extra.c"])

        data = run("--coverage-exclude", "lib/src/*.c")
        self.assertEqual(
            [f["file"] for f in data["files"]],
            ["lib/include/covlib.h", "lib/src/util/extra.c"])

        output = self.strip_ansi(self.bake(
            [local_env, "coverage-report", "--coverage-root", "lib",
             "--coverage-include", "src", "--coverage-exclude", "src/util/*"],
            cwd=root))
        self.assertIn(f"coverage of 1 test project: {test_id}", output)
        self.assertRegex(output, r"\n  src\s+66\.67%\s+6/9")
        self.assertNotIn("util", output)
        self.assertNotIn("include", output)
        report_dir = root / ".bake" / "local_env" / "coverage" / "coverage_report"
        combined = json.loads((report_dir / "coverage.json").read_text())
        self.assertEqual(combined["projects"], [test_id])
        self.assertEqual([f["file"] for f in combined["files"]], ["src/covlib.c"])
        self.assertEqual(combined["lines"], by_file["lib/src/covlib.c"]["lines"])
        index = (report_dir / "index.html").read_text()
        self.assertIn('"files": [\n["src/covlib.c",[6,9],[1,2],', index)

    def test_coverage_report_options_are_validated(self) -> None:
        output = self.strip_ansi(self.bake_expect_failure(
            ["build", "test/projects/c/app_helloworld", "--coverage",
             "--coverage-include", "src"]))
        self.assertIn(
            "--coverage-root, --coverage-include and --coverage-exclude can only "
            "be used with the run, test and coverage-report commands", output)

        output = self.strip_ansi(self.bake_expect_failure(
            ["run", "test/projects/c/app_helloworld", "--coverage-root", "."]))
        self.assertIn("require --coverage for the run command", output)

    def test_coverage_requires_clang(self) -> None:
        if platform.system() == "Windows":
            self.skipTest("coverage is not supported on Windows")
        stamp = int(time.time() * 1_000_000)
        fake_dir = self.repo_root / "test" / "tmp" / f"fake_gcc_{stamp}"
        self.addCleanup(shutil.rmtree, fake_dir, ignore_errors=True)
        fake_dir.mkdir(parents=True, exist_ok=True)
        fake_gcc = fake_dir / "gcc"
        fake_gcc.write_text("#!/bin/sh\necho 'gcc (GCC) 14.2.0'\n")
        fake_gcc.chmod(0o755)

        output = self.bake_expect_failure(
            ["build", "test/projects/c/app_helloworld", "--coverage",
             "--cc", str(fake_gcc)])
        self.assertIn("--coverage requires clang", output)

        output = self.bake_expect_failure(["clean", "--coverage"])
        self.assertIn("--coverage can only be used with", output)

    def timeout_project_files(self, project_dir: Path, project_id: str) -> None:
        """Write a test project with a hung case, a slow case and a quiet case."""
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "test",
                    "test": {
                        "testsuites": [
                            {"id": "Alpha", "testcases": ["quick", "hang", "quiet"]},
                            {"id": "Beta", "testcases": ["hang"], "timeout": 2},
                        ]
                    },
                },
                indent=4,
            ) + "\n"
        )

        case_header = (
            "#include <bake_test.h>\n"
            "#include <stdio.h>\n"
            "#include <stdlib.h>\n"
            "#include <string.h>\n"
            "#if defined(_WIN32)\n"
            "#include <process.h>\n"
            "#include <windows.h>\n"
            "#define case_sleep(sec) Sleep((DWORD)(sec) * 1000)\n"
            "#define case_pid() ((long)_getpid())\n"
            "#else\n"
            "#include <unistd.h>\n"
            "#define case_sleep(sec) sleep(sec)\n"
            "#define case_pid() ((long)getpid())\n"
            "#endif\n"
            "static void write_pid(const char *name, long pid) {\n"
            "    const char *dir = getenv(\"BAKE_TIMEOUT_PID_DIR\");\n"
            "    char path[1024];\n"
            "    snprintf(path, sizeof(path), \"%s/%s\", dir ? dir : \".\", name);\n"
            "    FILE *f = fopen(path, \"w\");\n"
            "    if (f) { fprintf(f, \"%ld\\n\", pid); fclose(f); }\n"
            "}\n"
        )

        (src_dir / "Alpha.c").write_text(
            case_header +
            "void Alpha_quick(void) { test_assert(true); }\n"
            "void Alpha_hang(void) {\n"
            "#if !defined(_WIN32)\n"
            "    pid_t child = fork();\n"
            "    if (!child) { case_sleep(300); _exit(0); }\n"
            "    write_pid(\"hang_child.pid\", (long)child);\n"
            "#endif\n"
            "    write_pid(\"hang_case.pid\", case_pid());\n"
            "    case_sleep(300);\n"
            "    test_assert(true);\n"
            "}\n"
            "void Alpha_quiet(void) {\n"
            "    case_sleep(2);\n"
            "    printf(\"QUIET DONE\\n\");\n"
            "    test_assert(true);\n"
            "}\n"
        )
        (src_dir / "Beta.c").write_text(
            case_header +
            "void Beta_hang(void) {\n"
            "    case_sleep(300);\n"
            "    test_assert(true);\n"
            "}\n"
        )

    def test_test_harness_times_out_hung_cases(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.timeout.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_timeout_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        self.timeout_project_files(project_dir, project_id)

        local_env = "--local-env=case_timeout"
        self.bake([local_env, "build", "."], cwd=project_dir)

        env = self.env.copy()
        env["BAKE_TIMEOUT_PID_DIR"] = str(project_dir)

        report = project_dir / "report.json"
        start = time.monotonic()
        output = self.strip_ansi(self.bake_expect_failure(
            [local_env, "run", ".", "--",
             "-j", "4", "--timeout", "4", "--json", str(report)],
            cwd=project_dir, env=env))
        elapsed = time.monotonic() - start

        self.assertLess(elapsed, 30.0, f"run took {elapsed:.1f}s, output:\n{output}")
        self.assertIn("TIMEOUT Alpha.hang (exceeded 4 seconds)", output)
        self.assertIn("TIMEOUT Beta.hang (exceeded 4 seconds)", output)
        self.assertIn(" - Alpha.hang (timeout)", output)
        self.assertIn("QUIET DONE", output)

        data = json.loads(report.read_text())
        self.assertEqual(data["fail"], 2)
        self.assertEqual(data["timeout"], 2)
        by_case = {(t["suite"], t["case"]): t for t in data["tests"]}
        self.assertEqual(by_case[("Alpha", "hang")]["status"], "timeout")
        self.assertEqual(by_case[("Beta", "hang")]["status"], "timeout")
        self.assertEqual(by_case[("Alpha", "quick")]["status"], "pass")
        self.assertEqual(by_case[("Alpha", "quiet")]["status"], "pass")
        self.assertLess(by_case[("Alpha", "hang")]["elapsed"], 20.0)

        if platform.system() != "Windows":
            for name in ("hang_case.pid", "hang_child.pid"):
                pid = int((project_dir / name).read_text().strip())
                with self.assertRaises(
                        ProcessLookupError, msg=f"{name} {pid} still running"):
                    os.kill(pid, 0)

        start = time.monotonic()
        suite_output = self.strip_ansi(self.bake_expect_failure(
            [local_env, "run", ".", "--", "Beta"], cwd=project_dir, env=env))
        suite_elapsed = time.monotonic() - start

        self.assertIn("TIMEOUT Beta.hang (exceeded 2 seconds)", suite_output)
        self.assertLess(suite_elapsed, 30.0, f"suite run took {suite_elapsed:.1f}s")

    def test_test_harness_waits_for_cases_without_burning_cpu(self) -> None:
        if platform.system() == "Windows":
            self.skipTest("getrusage is not available on Windows")

        import resource

        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.idle.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_idle_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        cases = [f"sleep_{i}" for i in range(1, 5)]
        (project_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "test",
                    "value": {"output": "harness_idle"},
                    "test": {"testsuites": [{"id": "Sleeper", "testcases": cases}]},
                },
                indent=4,
            ) + "\n"
        )
        (src_dir / "Sleeper.c").write_text(
            "#include <bake_test.h>\n"
            "#include <unistd.h>\n"
            + "".join(
                f"void Sleeper_{case}(void) {{ sleep(4); test_assert(true); }}\n"
                for case in cases
            )
        )

        local_env = "--local-env=case_idle"
        self.bake([local_env, "build", "."], cwd=project_dir)
        binaries = sorted(
            path for path in project_dir.rglob(f"harness_idle{EXE_SUFFIX}")
            if path.is_file())
        self.assertTrue(binaries, "test binary was not built")
        exe = binaries[0]

        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.monotonic()
        proc = subprocess.run(
            [str(exe), "-j", "4", "--timeout", "60"],
            cwd=str(project_dir), env=self.env, text=True,
            capture_output=True, check=False)
        elapsed = time.monotonic() - start
        after = resource.getrusage(resource.RUSAGE_CHILDREN)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertGreater(elapsed, 3.0)
        cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
        self.assertLess(
            cpu, 1.0,
            f"runner used {cpu:.3f}s of cpu while waiting {elapsed:.1f}s on sleeping cases")

    def bench_project_files(self, project_dir: Path, project_id: str, cases: list[str]) -> None:
        """Write a bench project with hand written benchcase bodies."""
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "application",
                    "bench": {
                        "benchsuites": [{"id": "Alpha", "benchcases": cases}]
                    },
                },
                indent=4,
            ) + "\n"
        )
        (src_dir / "Alpha.c").write_text(
            "#include <bake_bench.h>\n"
            "\n"
            "void Alpha_add(bench_t *b) {\n"
            "    int x = 0;\n"
            "    bench_set_items(b, 2);\n"
            "    while (bench_iter(b)) {\n"
            "        x += 3;\n"
            "        bench_keep(x);\n"
            "        bench_counter(b, \"adds\", 1.0);\n"
            "    }\n"
            "}\n"
            "\n"
            "void Alpha_mul(bench_t *b) {\n"
            "    int x = 1;\n"
            "    while (bench_iter(b)) {\n"
            "        x *= 3;\n"
            "        bench_keep(x);\n"
            "    }\n"
            "}\n"
        )

    def test_bench_project_generates_main_and_stubs(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.bench.gen.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"bench_gen_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "application",
                    "bench": {
                        "benchsuites": [{
                            "id": "Alpha",
                            "benchcases": ["add", "mul"],
                            "setup": True,
                            "teardown": True,
                        }]
                    },
                },
                indent=4,
            ) + "\n"
        )

        self.bake(["--local-env=bench_gen", "build", "."], cwd=project_dir)

        info = self.strip_ansi(self.bake(["--local-env=bench_gen", "info", "."], cwd=project_dir))
        self.assertRegex(info, r"(?m)^kind:\s+bench$")

        main_src = (src_dir / "main.c").read_text()
        self.assertIn("#include <bake_bench.h>", main_src)
        self.assertIn("void Alpha_setup(void);", main_src)
        self.assertIn("void Alpha_teardown(void);", main_src)
        self.assertIn("void Alpha_add(bench_t *b);", main_src)
        self.assertIn("void Alpha_mul(bench_t *b);", main_src)
        self.assertIn("bake_bench_case Alpha_benchcases[] = {", main_src)
        self.assertIn("bake_bench_suite suites[] = {", main_src)
        self.assertIn(f'bake_bench_run("{project_id}", argc, argv, suites, 1)', main_src)

        suite_src = (src_dir / "Alpha.c").read_text()
        self.assertIn("#include <bake_bench.h>", suite_src)
        self.assertIn("void Alpha_setup(void) {", suite_src)
        self.assertIn("void Alpha_teardown(void) {", suite_src)
        self.assertIn("void Alpha_add(bench_t *b) {", suite_src)
        self.assertIn("while (bench_iter(b)) {", suite_src)

        generated = sorted(project_dir.glob("**/generated/bake_bench.h"))
        self.assertTrue(generated, "expected bake_bench.h to be copied into the generated dir")

    def test_bench_harness_times_out_hung_cases(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.bench.timeout.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"bench_timeout_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "application",
                    "bench": {
                        "benchsuites": [{"id": "Alpha", "benchcases": ["hang", "after"]}]
                    },
                },
                indent=4,
            ) + "\n"
        )
        (src_dir / "Alpha.c").write_text(
            "#include <bake_bench.h>\n"
            "#if defined(_WIN32)\n"
            "#include <windows.h>\n"
            "#define case_sleep(sec) Sleep((DWORD)(sec) * 1000)\n"
            "#else\n"
            "#include <unistd.h>\n"
            "#define case_sleep(sec) sleep(sec)\n"
            "#endif\n"
            "void Alpha_hang(bench_t *b) { (void)b; case_sleep(300); }\n"
            "void Alpha_after(bench_t *b) {\n"
            "    int x = 0;\n"
            "    while (bench_iter(b)) { x += 1; bench_keep(x); }\n"
            "}\n"
        )

        report = project_dir / "report.json"
        start = time.monotonic()
        output = self.strip_ansi(self.bake_expect_failure(
            ["--local-env=bench_timeout", "run", ".", "--", "--timeout", "2",
             "--time", "0.1", "--samples", "3", "--sample-time", "0.001",
             "--json", str(report)],
            cwd=project_dir))
        elapsed = time.monotonic() - start

        self.assertIn("TIMEOUT Alpha.hang (exceeded 2 seconds)", output)
        self.assertRegex(output, r"(?m)^Alpha\.after\s+\S+ ns/iter")
        self.assertIn("FAILED Alpha.hang (timeout)", output)
        self.assertLess(elapsed, 60.0, f"bench run took {elapsed:.1f}s")

        data = json.loads(report.read_text())
        self.assertEqual([b["case"] for b in data["benchmarks"]], ["after"])
        self.assertEqual(data["failed"], 1)
        self.assertEqual(data["failures"][0]["case"], "hang")
        self.assertEqual(data["failures"][0]["status"], "timeout")

    def test_bench_harness_isolates_crashing_cases(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.bench.crash.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"bench_crash_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        self.bench_project_files(project_dir, project_id, ["add", "boom", "nosamples", "mul"])
        with (project_dir / "src" / "Alpha.c").open("a") as f:
            f.write(
                "\n"
                "#include <stdio.h>\n"
                "void Alpha_boom(bench_t *b) {\n"
                "    volatile int *ptr = NULL;\n"
                "    (void)b;\n"
                "    printf(\"about to crash\\n\");\n"
                "    fflush(stdout);\n"
                "    *ptr = 1;\n"
                "}\n"
                "\n"
                "void Alpha_nosamples(bench_t *b) { (void)b; }\n"
            )

        report = project_dir / "report.json"
        output = self.strip_ansi(self.bake_expect_failure(
            ["--local-env=bench_crash", "run", ".", "--",
             "--time", "0.1", "--samples", "3", "--sample-time", "0.001",
             "--json", str(report)],
            cwd=project_dir))

        self.assertIn("about to crash", output)
        self.assertIn("CRASH Alpha.boom", output)
        self.assertIn("ERROR Alpha.nosamples (exit code 1)", output)
        self.assertRegex(output, r"(?m)^Alpha\.add\s+\S+ ns/iter")
        self.assertRegex(output, r"(?m)^Alpha\.mul\s+\S+ ns/iter")
        self.assertIn("2 benchmark(s) failed:", output)
        self.assertIn("FAILED Alpha.boom (crash)", output)
        self.assertIn("FAILED Alpha.nosamples (error)", output)
        self.assertLess(output.index("Alpha.add "), output.index("CRASH Alpha.boom"))
        self.assertLess(output.index("CRASH Alpha.boom"), output.index("Alpha.mul "))

        data = json.loads(report.read_text())
        self.assertEqual(data["isolation"], "process")
        self.assertEqual(data["cases"], 2)
        self.assertEqual(data["failed"], 2)
        by_case = {b["case"]: b for b in data["benchmarks"]}
        self.assertEqual(set(by_case), {"add", "mul"})
        for case in by_case.values():
            self.assertEqual(case["status"], "ok")
            self.assertNotIn("sample_ns", case)
        self.assertEqual(by_case["add"]["items_per_iter"], 2)
        counters = {c["name"]: c for c in by_case["add"]["counters"]}
        self.assertAlmostEqual(counters["adds"]["per_iter"], 1.0, places=3)

        failures = {f["case"]: f for f in data["failures"]}
        self.assertEqual(set(failures), {"boom", "nosamples"})
        self.assertEqual(failures["boom"]["status"], "crash")
        if platform.system() != "Windows":
            self.assertIn(failures["boom"]["signal"], {signal.SIGSEGV, signal.SIGBUS})
        self.assertEqual(failures["nosamples"]["status"], "error")
        self.assertEqual(failures["nosamples"]["exit_code"], 1)

        single = project_dir / "single.json"
        self.bake_expect_failure(
            ["--local-env=bench_crash", "run", ".", "--", "Alpha.boom",
             "--time", "0.1", "--samples", "3", "--sample-time", "0.001",
             "--json", str(single)],
            cwd=project_dir)
        data = json.loads(single.read_text())
        self.assertEqual(data["benchmarks"], [])
        self.assertEqual([f["case"] for f in data["failures"]], ["boom"])

    def test_bench_harness_runs_in_process(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.bench.inproc.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"bench_inproc_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        self.bench_project_files(project_dir, project_id, ["add", "mul"])

        report = project_dir / "report.json"
        output = self.strip_ansi(self.bake(
            ["--local-env=bench_inproc", "run", ".", "--", "--in-process",
             "--time", "0.1", "--samples", "3", "--sample-time", "0.001",
             "--json", str(report)],
            cwd=project_dir))
        self.assertIn("Alpha.add", output)
        self.assertIn("Alpha.mul", output)

        data = json.loads(report.read_text())
        self.assertEqual(data["isolation"], "none")
        self.assertEqual(data["failed"], 0)
        self.assertEqual(data["failures"], [])
        self.assertEqual({b["case"] for b in data["benchmarks"]}, {"add", "mul"})

    def test_bench_project_runs_cases_and_writes_json_report(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.bench.run.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"bench_run_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        self.bench_project_files(project_dir, project_id, ["add", "mul"])

        report = project_dir / "report.json"
        output = self.strip_ansi(self.bake(
            ["--local-env=bench_run", "run", ".", "--",
             "--time", "0.2", "--samples", "5", "--sample-time", "0.001",
             "--json", str(report)],
            cwd=project_dir))

        self.assertIn("Alpha.add", output)
        self.assertIn("Alpha.mul", output)
        self.assertIn("ns/iter", output)
        self.assertIn("ci95", output)
        self.assertIn("outliers", output)

        self.assertTrue(report.is_file(), f"missing report {report}")
        data = json.loads(report.read_text())
        self.assertEqual(data["project"], project_id)
        self.assertEqual(data["tool"], "bake3")
        self.assertTrue(data["tool_version"])
        self.assertTrue(data["timestamp"].endswith("Z"))
        self.assertIn(data["host"]["os"], {"Darwin", "Linux", "Windows"})
        self.assertTrue(data["host"]["arch"])
        self.assertGreaterEqual(data["host"]["cpu_count"], 1)
        self.assertEqual(data["cases"], 2)
        self.assertGreater(data["time_sec"], 0.0)

        by_case = {b["case"]: b for b in data["benchmarks"]}
        self.assertEqual(set(by_case), {"add", "mul"})
        for name, case in by_case.items():
            self.assertEqual(case["suite"], "Alpha")
            self.assertGreaterEqual(case["iterations"], 1)
            self.assertGreaterEqual(case["samples"], 1)
            self.assertLessEqual(case["samples"], 5)
            self.assertNotIn("sample_ns", case)
            self.assertEqual(
                case["total_iterations"], case["iterations"] * case["samples"])
            self.assertLessEqual(case["min_ns"], case["median_ns"])
            self.assertLessEqual(case["median_ns"], case["max_ns"])
            self.assertLessEqual(case["ci_low_ns"], case["mean_ns"])
            self.assertLessEqual(case["mean_ns"], case["ci_high_ns"])
            self.assertLessEqual(case["min_ns"], case["p95_ns"])
            self.assertLessEqual(case["p95_ns"], case["p99_ns"])
            self.assertGreaterEqual(case["stddev_ns"], 0.0)
            self.assertEqual(case["ci_level"], 0.95)
            self.assertTrue(case["ci_method"])
            self.assertGreaterEqual(case["outliers"], 0)
            self.assertGreaterEqual(case["outliers_severe"], 0)
            self.assertGreater(case["time_sec"], 0.0)
            self.assertGreater(case["min_ns"], 0.0)

        self.assertEqual(by_case["add"]["items_per_iter"], 2)
        self.assertGreater(by_case["add"]["items_per_sec"], 0.0)
        counters = {c["name"]: c for c in by_case["add"]["counters"]}
        self.assertIn("adds", counters)
        self.assertGreater(counters["adds"]["total"], 0.0)
        self.assertAlmostEqual(counters["adds"]["per_iter"], 1.0, places=3)
        self.assertEqual(by_case["mul"]["counters"], [])

    def test_bench_project_filters_cases(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.bench.filter.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"bench_filter_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        self.bench_project_files(project_dir, project_id, ["add", "mul"])

        self.bake(["--local-env=bench_filter", "build", "."], cwd=project_dir)

        filtered = project_dir / "filtered.json"
        output = self.strip_ansi(self.bake(
            ["--local-env=bench_filter", "run", ".", "--",
             "--time", "0.1", "--samples", "3", "--sample-time", "0.001",
             "--filter", "mul", "--json", str(filtered)],
            cwd=project_dir))
        self.assertIn("Alpha.mul", output)
        self.assertNotIn("Alpha.add", output)
        data = json.loads(filtered.read_text())
        self.assertEqual([b["case"] for b in data["benchmarks"]], ["mul"])

        single = project_dir / "single.json"
        self.bake(
            ["--local-env=bench_filter", "run", ".", "--", "Alpha.add",
             "--time", "0.1", "--samples", "3", "--sample-time", "0.001",
             "--json", str(single)],
            cwd=project_dir)
        data = json.loads(single.read_text())
        self.assertEqual([b["case"] for b in data["benchmarks"]], ["add"])

        self.bake_expect_failure(
            ["--local-env=bench_filter", "run", ".", "--", "--filter", "nope",
             "--time", "0.1", "--samples", "3", "--sample-time", "0.001"],
            cwd=project_dir)

    def test_bench_project_compares_against_baseline(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.bench.baseline.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"bench_baseline_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        self.bench_project_files(project_dir, project_id, ["add", "mul"])

        baseline = project_dir / "baseline.json"
        bench_args = [
            "--time", "0.1", "--samples", "3", "--sample-time", "0.001"]
        self.bake(
            ["--local-env=bench_baseline", "run", ".", "--",
             *bench_args, "--json", str(baseline)],
            cwd=project_dir)

        output = self.strip_ansi(self.bake(
            ["--local-env=bench_baseline", "run", ".", "--",
             *bench_args, "--baseline", str(baseline)],
            cwd=project_dir))
        self.assertIn("vs baseline", output)
        self.assertIn("0 regression(s)", output)

        faster = json.loads(baseline.read_text())
        for case in faster["benchmarks"]:
            case["median_ns"] = case["median_ns"] / 4.0
        faster_path = project_dir / "faster.json"
        faster_path.write_text(json.dumps(faster, indent=2))

        output = self.strip_ansi(self.bake(
            ["--local-env=bench_baseline", "run", ".", "--",
             *bench_args, "--baseline", str(faster_path), "--threshold", "0.05"],
            cwd=project_dir))
        self.assertIn("(regression)", output)
        self.assertIn("REGRESSION Alpha.add", output)
        self.assertIn("2 regression(s)", output)

        output = self.strip_ansi(self.bake_expect_failure(
            ["--local-env=bench_baseline", "run", ".", "--",
             *bench_args, "--baseline", str(faster_path),
             "--fail-on-regression"],
            cwd=project_dir))
        self.assertIn("REGRESSION Alpha.mul", output)

    @staticmethod
    def report_steps(report: dict) -> list[dict]:
        """Flatten the step tree of a build report into a list."""
        found: list[dict] = []

        def walk(steps: list[dict]) -> None:
            for step in steps:
                found.append(step)
                walk(step["children"])

        walk(report["steps"])
        return found

    def write_build_json_app(self, name: str) -> tuple[Path, str]:
        project_dir, app_id = self.write_simple_app_project(
            name,
            "#include <stdio.h>\n"
            "int helper(void);\n"
            "int main(void) {\n"
            '    printf("build json %d\\n", helper());\n'
            "    return 0;\n"
            "}\n",
        )
        (project_dir / "src" / "helper.c").write_text("int helper(void) { return 2; }\n")
        return project_dir, app_id

    def test_build_json_writes_a_native_build_report(self) -> None:
        project_dir, app_id = self.write_build_json_app("build_json_native")
        report_path = project_dir / "reports" / "build.json"

        self.bake(["build", str(project_dir), "--build-json", str(report_path)])

        self.assertTrue(report_path.is_file(), f"Expected build report at {report_path}")
        report = json.loads(report_path.read_text())

        self.assertEqual(report["tool"], "bake3")
        self.assertTrue(report["tool_version"])
        self.assertRegex(report["timestamp"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertEqual(report["host"]["os"], platform.system())
        self.assertEqual(report["host"]["arch"], self.host_arch())
        self.assertGreater(report["host"]["cpu_count"], 0)
        self.assertEqual(report["cfg"], "debug")
        self.assertEqual(report["target"], f"{self.host_arch()}-{platform.system()}")
        self.assertEqual(report["environment"], "global")
        self.assertTrue(report["ok"])
        self.assertGreater(report["total_sec"], 0.0)

        steps = self.report_steps(report)
        self.assertIn("discovery", {step["kind"] for step in steps})

        compiles = [step for step in steps if step["kind"] == "compile"]
        self.assertEqual(
            sorted(step["name"] for step in compiles),
            ["src/helper.c", "src/main.c"])
        for step in compiles:
            self.assertTrue(step["ok"])
            self.assertEqual(step["project"], app_id)
            self.assertGreater(step["duration_sec"], 0.0)
            self.assertGreaterEqual(step["start_sec"], 0.0)
            self.assertTrue(
                step["object"].endswith(OBJ_SUFFIX),
                f"Expected an object path on a compile step, got {step['object']}")

        links = [step for step in steps if step["kind"] == "link"]
        self.assertEqual(len(links), 1)
        self.assertGreater(links[0]["duration_sec"], 0.0)

        project_steps = [step for step in steps if step["kind"] == "project"]
        self.assertEqual([step["name"] for step in project_steps], [app_id])
        child_kinds = {child["kind"] for child in project_steps[0]["children"]}
        self.assertIn("compile", child_kinds)
        self.assertIn("link", child_kinds)

        totals = report["totals"]
        self.assertEqual(
            set(totals["kind"]),
            {"compile", "link", "bundle", "discovery", "generate", "etc", "loc", "other"})
        self.assertGreater(totals["kind"]["compile"], 0.0)
        self.assertGreater(totals["kind"]["link"], 0.0)

        project_totals = totals["project"][app_id]
        self.assertEqual(project_totals["files"], 2)
        self.assertGreater(project_totals["compile_sec"], 0.0)
        self.assertGreater(project_totals["link_sec"], 0.0)
        self.assertGreater(project_totals["total_sec"], 0.0)

    def test_build_json_reports_a_failing_compile(self) -> None:
        project_dir, app_id = self.write_simple_app_project(
            "build_json_failure",
            "int main(void) { this is not c }\n",
        )
        report_path = project_dir / "build.json"

        self.bake_expect_failure(
            ["build", str(project_dir), "--build-json", str(report_path)])

        self.assertTrue(report_path.is_file(), f"Expected build report at {report_path}")
        report = json.loads(report_path.read_text())
        self.assertFalse(report["ok"])

        steps = self.report_steps(report)
        failed = [
            step for step in steps if step["kind"] == "compile" and not step["ok"]]
        self.assertEqual([step["name"] for step in failed], ["src/main.c"])
        self.assertTrue(failed[0]["error"], "Expected error text on the failing step")
        self.assertEqual(failed[0]["project"], app_id)

        project_step = next(step for step in steps if step["kind"] == "project")
        self.assertFalse(project_step["ok"])

    def test_build_json_records_git_of_the_workspace(self) -> None:
        project_dir, _ = self.write_build_json_app("build_json_git")
        report_path = project_dir / "build.json"

        self.bake(["build", str(project_dir), "--build-json", str(report_path)])

        report = json.loads(report_path.read_text())
        sha = self.run_cmd(["git", "rev-parse", "HEAD"]).strip()
        branch = self.run_cmd(["git", "rev-parse", "--abbrev-ref", "HEAD"]).strip()
        self.assertEqual(report["git"]["sha"], sha)
        self.assertEqual(report["git"]["branch"], branch)
        self.assertIn(report["git"]["dirty"], (True, False))

        outside = Path(tempfile.mkdtemp(prefix="bake_build_json_"))
        self.addCleanup(shutil.rmtree, outside, ignore_errors=True)
        (outside / "src").mkdir(parents=True)
        (outside / "project.json").write_text(
            '{\n    "id": "build_json_no_git",\n    "type": "application"\n}\n')
        (outside / "src" / "main.c").write_text("int main(void) { return 0; }\n")

        self.bake(["build", ".", "--build-json", "build.json"], cwd=outside)

        outside_report = json.loads((outside / "build.json").read_text())
        self.assertIsNone(
            outside_report["git"],
            "A workspace outside a git repository must report a null git field")

    def test_build_json_is_accepted_by_run_and_test(self) -> None:
        app_dir, _ = self.write_build_json_app("build_json_run")
        run_report = app_dir / "run.json"

        output = self.strip_ansi(
            self.bake(["run", str(app_dir), "--build-json", str(run_report)]))
        self.assertIn("build json 2", output)

        report = json.loads(run_report.read_text())
        self.assertTrue(report["ok"])
        self.assertTrue(
            [step for step in self.report_steps(report) if step["kind"] == "link"],
            "Expected a link step in the report of a run command")

        stamp = int(time.time() * 1_000_000)
        project_id = f"build_json_test_{stamp}"
        test_dir = self.repo_root / "test" / "tmp" / f"build_json_test_{stamp}"
        self.addCleanup(shutil.rmtree, test_dir, ignore_errors=True)
        (test_dir / "src").mkdir(parents=True)
        (test_dir / "project.json").write_text(
            json.dumps(
                {
                    "id": project_id,
                    "type": "test",
                    "test": {"testsuites": [{"id": "Alpha", "testcases": ["ok"]}]},
                },
                indent=4,
            ) + "\n"
        )
        (test_dir / "src" / "Alpha.c").write_text(
            "#include <bake_test.h>\n"
            "void Alpha_ok(void) { test_assert(true); }\n"
        )

        test_report = test_dir / "test.json"
        self.bake(
            ["--local-env=build_json_env", "test", ".",
             "--build-json", str(test_report)],
            cwd=test_dir)

        report = json.loads(test_report.read_text())
        self.assertTrue(report["ok"])
        self.assertEqual(report["environment"], "build_json_env")
        generated = {
            step["name"] for step in self.report_steps(report)
            if step["kind"] == "generate"}
        self.assertIn("test harness main", generated)
        self.assertGreater(report["totals"]["project"][project_id]["files"], 0)

    def test_build_json_is_rejected_for_other_commands(self) -> None:
        report_path = self.repo_root / "test" / "tmp" / "unused_build_report.json"

        output = self.strip_ansi(self.bake_expect_failure(
            ["clean", "test", "--build-json", str(report_path)]))

        self.assertIn("--build-json can only be used with", output)
        self.assertFalse(report_path.exists())

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_build_json_reports_the_wasm_target(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, app_id = self.write_simple_app_project(
            "build_json_em",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
            lang_c='{"embed": ["assets"]}',
        )
        (project_dir / "assets").mkdir()
        (project_dir / "assets" / "data.txt").write_text("asset\n")
        report_path = project_dir / "build.json"

        self.bake(
            ["--target", "em", "build", str(project_dir),
             "--build-json", str(report_path)])

        report = json.loads(report_path.read_text())
        self.assertEqual(report["target"], "em")
        self.assertTrue(report["ok"])

        steps = self.report_steps(report)
        compiles = [step for step in steps if step["kind"] == "compile"]
        self.assertEqual([step["name"] for step in compiles], ["src/main.c"])

        links = [step for step in steps if step["kind"] == "link"]
        self.assertEqual(len(links), 1)
        self.assertGreater(links[0]["duration_sec"], 0.0)
        self.assertEqual([child["name"] for child in links[0]["children"]], ["embed"])
        self.assertEqual(report["totals"]["project"][app_id]["files"], 1)

    def test_build_json_reports_lines_of_code(self) -> None:
        project_dir, app_id = self.write_build_json_app("build_json_loc")
        (project_dir / "include").mkdir()
        (project_dir / "include" / "helper.h").write_text(
            "#ifndef HELPER_H\n#define HELPER_H\n\nint helper(void);\n\n#endif\n")
        report_path = project_dir / "build.json"

        self.bake(["build", str(project_dir), "--build-json", str(report_path)])

        report = json.loads(report_path.read_text())
        self.assertTrue(report["ok"])

        steps = self.report_steps(report)
        loc_steps = [step for step in steps if step["kind"] == "loc"]
        self.assertEqual([step["project"] for step in loc_steps], [app_id])
        self.assertTrue(loc_steps[0]["ok"])
        self.assertGreater(loc_steps[0]["duration_sec"], 0.0)

        self.assertIn("loc", report["totals"]["kind"])
        self.assertGreater(report["totals"]["kind"]["loc"], 0.0)

        project_loc = report["totals"]["project"][app_id]["loc"]
        self.assertEqual(project_loc["files"], 3)
        self.assertGreater(project_loc["code"], 0)
        self.assertIn("C", project_loc["by_language"])

        workspace_loc = report["totals"]["loc"]
        self.assertEqual(workspace_loc["files"], project_loc["files"])
        self.assertEqual(workspace_loc["code"], project_loc["code"])
        self.assertEqual(workspace_loc["by_language"], project_loc["by_language"])

    def test_build_json_reports_lines_of_code_per_file(self) -> None:
        project_dir, app_id = self.write_build_json_app("build_json_loc_by_file")
        (project_dir / "include" / "sub").mkdir(parents=True)
        (project_dir / "include" / "helper.h").write_text(
            "// helper api\n#ifndef HELPER_H\n#define HELPER_H\n\n"
            "int helper(void);\n\n#endif\n")
        (project_dir / "include" / "sub" / "detail.h").write_text(
            "#define DETAIL 1\n")
        report_path = project_dir / "build.json"

        self.bake(["build", str(project_dir), "--build-json", str(report_path)])

        report = json.loads(report_path.read_text())
        self.assertTrue(report["ok"])

        project_loc = report["totals"]["project"][app_id]["loc"]
        by_file = project_loc["by_file"]
        self.assertEqual(
            sorted(by_file),
            ["include/helper.h", "include/sub/detail.h", "src/helper.c", "src/main.c"])
        self.assertEqual(
            by_file["include/helper.h"],
            {"language": "C/C++ Header", "code": 4, "comment": 1, "blank": 2})
        self.assertEqual(
            by_file["include/sub/detail.h"],
            {"language": "C/C++ Header", "code": 1, "comment": 0, "blank": 0})
        self.assertEqual(by_file["src/helper.c"]["language"], "C")
        self.assertEqual(by_file["src/helper.c"]["code"], 1)

        self.assertEqual(project_loc["files"], len(by_file))
        for field in ("code", "comment", "blank"):
            self.assertEqual(
                project_loc[field], sum(entry[field] for entry in by_file.values()))
        self.assertEqual(project_loc["by_language"]["C/C++ Header"]["files"], 2)
        self.assertEqual(project_loc["by_language"]["C"]["files"], 2)

        self.assertNotIn("by_file", report["totals"]["loc"])

    def test_build_json_reports_a_note_when_cloc_is_unavailable(self) -> None:
        project_dir, app_id = self.write_build_json_app("build_json_loc_missing")
        report_path = project_dir / "build.json"

        cloc_path = shutil.which("cloc")
        cloc_dir = os.path.realpath(os.path.dirname(cloc_path)) if cloc_path else None
        parts = [
            entry for entry in self.env.get("PATH", "").split(os.pathsep)
            if entry and os.path.realpath(entry) != cloc_dir
        ]
        env = dict(self.env)
        env["PATH"] = os.pathsep.join(parts)

        self.bake(
            ["build", str(project_dir), "--build-json", str(report_path)],
            env=env)

        report = json.loads(report_path.read_text())
        self.assertTrue(report["ok"])
        self.assertNotIn("loc", report["totals"]["project"][app_id])
        self.assertEqual(report["totals"]["loc"], {"note": "cloc not found"})

        steps = self.report_steps(report)
        loc_steps = [step for step in steps if step["kind"] == "loc"]
        self.assertEqual(len(loc_steps), 1)
        self.assertFalse(loc_steps[0]["ok"])
        self.assertEqual(loc_steps[0]["error"], "cloc not found")

    def test_build_json_repeat_keeps_the_median_run(self) -> None:
        project_dir, app_id = self.write_build_json_app("build_json_repeat")
        report_path = project_dir / "reports" / "build.json"

        output = self.strip_ansi(self.bake(
            ["rebuild", str(project_dir), "--repeat", "3", "--warmup", "1",
             "--build-json", str(report_path)]))
        self.assertEqual(output.count("clean] "), 4)
        self.assertIn("[ repeat] median of 3 runs", output)

        report = json.loads(report_path.read_text())
        self.assertTrue(report["ok"])
        self.assertEqual(
            sorted(p.name for p in report_path.parent.iterdir()), ["build.json"])

        repeat = report["repeat"]
        self.assertEqual(repeat["warmup"], 1)
        self.assertEqual(repeat["count"], 3)
        runs = repeat["runs"]
        self.assertEqual([run["run"] for run in runs], [0, 1, 2, 3])
        self.assertEqual(
            [run["warmup"] for run in runs], [True, False, False, False])
        for run in runs:
            self.assertTrue(run["ok"])
            self.assertEqual(run["exit_code"], 0)
            self.assertGreater(run["total_sec"], 0.0)
        for run in runs[1:]:
            self.assertEqual(run["loc_sec"], 0)

        measured = sorted(run["total_sec"] - run["loc_sec"] for run in runs[1:])
        self.assertAlmostEqual(repeat["median_sec"], measured[1], places=6)
        chosen = runs[repeat["chosen_run"]]
        self.assertFalse(chosen["warmup"])
        self.assertAlmostEqual(chosen["total_sec"], report["total_sec"], places=6)
        self.assertAlmostEqual(
            chosen["total_sec"] - chosen["loc_sec"], repeat["median_sec"], places=6)

        steps = self.report_steps(report)
        self.assertEqual([step for step in steps if step["kind"] == "loc"], [])
        self.assertEqual(report["totals"]["kind"]["loc"], 0)
        self.assertEqual(
            sorted(step["name"] for step in steps if step["kind"] == "compile"),
            ["src/helper.c", "src/main.c"])
        self.assertEqual(report["totals"]["project"][app_id]["files"], 2)

        if shutil.which("cloc"):
            self.assertGreater(runs[0]["loc_sec"], 0.0)
            project_loc = report["totals"]["project"][app_id]["loc"]
            self.assertEqual(project_loc["files"], 2)
            self.assertEqual(
                sorted(project_loc["by_file"]), ["src/helper.c", "src/main.c"])
            self.assertEqual(report["totals"]["loc"]["files"], 2)

    def test_build_json_repeat_without_warmup_counts_loc_once(self) -> None:
        project_dir, app_id = self.write_build_json_app("build_json_repeat_nowarm")
        report_path = project_dir / "build.json"

        self.bake(
            ["build", str(project_dir), "--repeat", "2",
             "--build-json", str(report_path)])

        report = json.loads(report_path.read_text())
        self.assertTrue(report["ok"])
        repeat = report["repeat"]
        self.assertEqual((repeat["warmup"], repeat["count"]), (0, 2))
        runs = repeat["runs"]
        self.assertEqual([run["warmup"] for run in runs], [False, False])
        self.assertEqual(runs[1]["loc_sec"], 0)
        self.assertIn(repeat["chosen_run"], (0, 1))
        measured = sorted(run["total_sec"] - run["loc_sec"] for run in runs)
        self.assertAlmostEqual(repeat["median_sec"], measured[0], places=6)
        self.assertAlmostEqual(
            report["total_sec"] - report["totals"]["kind"]["loc"],
            repeat["median_sec"], places=6)
        if shutil.which("cloc"):
            self.assertGreater(runs[0]["loc_sec"], 0.0)
            self.assertEqual(report["totals"]["project"][app_id]["loc"]["files"], 2)

    def test_build_json_repeat_stops_at_a_failing_run(self) -> None:
        project_dir, _ = self.write_simple_app_project(
            "build_json_repeat_failure",
            "int main(void) { this is not c }\n",
        )
        report_path = project_dir / "build.json"

        self.bake_expect_failure(
            ["rebuild", str(project_dir), "--repeat", "3", "--warmup", "1",
             "--build-json", str(report_path)])

        report = json.loads(report_path.read_text())
        self.assertFalse(report["ok"])
        self.assertIsNone(report["repeat"]["chosen_run"])
        runs = report["repeat"]["runs"]
        self.assertEqual(len(runs), 1)
        self.assertFalse(runs[0]["ok"])
        self.assertNotEqual(runs[0]["exit_code"], 0)
        self.assertEqual(
            sorted(p.name for p in project_dir.glob("build.json*")), ["build.json"])

    def test_build_json_repeat_is_validated(self) -> None:
        report_path = self.repo_root / "test" / "tmp" / "unused_repeat_report.json"

        output = self.strip_ansi(self.bake_expect_failure(
            ["run", "test/projects/c/app_helloworld", "--repeat", "2",
             "--build-json", str(report_path)]))
        self.assertIn("--repeat and --warmup can only be used with", output)

        output = self.strip_ansi(self.bake_expect_failure(
            ["build", "test/projects/c/app_helloworld", "--repeat", "2"]))
        self.assertIn("--repeat and --warmup require --build-json", output)

        output = self.strip_ansi(self.bake_expect_failure(
            ["build", "test/projects/c/app_helloworld", "--repeat", "0",
             "--build-json", str(report_path)]))
        self.assertIn("invalid value for --repeat", output)
        self.assertFalse(report_path.exists())

    def test_setup_local_reinstalls_executable_bake_binary(self) -> None:
        installed_bake = self.bake_home / f"bake3{EXE_SUFFIX}"
        self.assertTrue(installed_bake.is_file(), f"Expected installed bake binary at {installed_bake}")

        if platform.system() == "Windows":
            self.bake(["setup", "--local"])
            self.assertTrue(
                installed_bake.is_file(),
                f"Expected setup to keep installed bake binary at {installed_bake}",
            )
            return

        self.assertTrue(
            installed_bake.stat().st_mode & stat.S_IXUSR,
            f"Expected installed bake binary to be executable: {installed_bake}",
        )

        installed_bake.chmod(installed_bake.stat().st_mode & ~0o111)
        self.assertFalse(
            installed_bake.stat().st_mode & stat.S_IXUSR,
            f"Test setup failed to remove execute bit from {installed_bake}",
        )
        inode_before = installed_bake.stat().st_ino

        self.bake(["setup", "--local"])

        self.assertTrue(
            installed_bake.stat().st_mode & stat.S_IXUSR,
            f"Expected setup to restore execute bit on {installed_bake}",
        )
        self.assertNotEqual(
            inode_before, installed_bake.stat().st_ino,
            "setup must replace the installed binary instead of overwriting it "
            "in place, which breaks processes that are running it",
        )
        self.run_cmd([str(installed_bake), "--help"])

    def test_local_env_routes_build_outputs_to_workspace_env(self) -> None:
        target = "test/projects/c/app_helloworld"
        project_id = "examples.c.app_helloworld"
        project_dir = self.repo_root / target
        local_env_home = self.repo_root / ".bake" / "local_env"
        project_local_bake = project_dir / ".bake"

        if local_env_home.exists():
            shutil.rmtree(local_env_home)
        if project_local_bake.exists():
            shutil.rmtree(project_local_bake)

        self.bake(["--local-env", "build", target])

        self.assertTrue(local_env_home.is_dir(), f"Expected local env dir at {local_env_home}")
        self.assertFalse(
            project_local_bake.exists(),
            f"Project-local build dir should not exist with --local-env: {project_local_bake}",
        )

        project_build_root = local_env_home / "build" / project_id
        self.assertTrue(
            project_build_root.is_dir(),
            f"Expected project build root in local env: {project_build_root}",
        )

        triplet_dirs = sorted(p for p in project_build_root.iterdir() if p.is_dir())
        self.assertTrue(triplet_dirs, f"Expected triplet build directories in {project_build_root}")
        build_dir = max(triplet_dirs, key=lambda p: p.stat().st_mtime_ns)

        self.assertTrue((build_dir / "obj").is_dir(), f"Expected obj dir in {build_dir}")
        self.assertTrue((build_dir / "generated").is_dir(), f"Expected generated dir in {build_dir}")
        artefacts = sorted(p for p in build_dir.iterdir() if p.is_file())
        self.assertTrue(artefacts, f"Expected build artefacts in {build_dir}")

    def test_local_env_overrides_preset_bake_home(self) -> None:
        target = "test/projects/c/app_helloworld"
        project_id = "examples.c.app_helloworld"
        local_env_home = self.repo_root / ".bake" / "local_env"
        override_home = self.repo_root / "test" / "tmp" / "bake_home_override_probe"

        if local_env_home.exists():
            shutil.rmtree(local_env_home)
        if override_home.exists():
            shutil.rmtree(override_home)

        env = self.env.copy()
        env["BAKE_HOME"] = str(override_home)
        env["BAKE_LOCAL_ENV"] = "0"

        self.bake(["--local-env", "build", target], env=env)

        self.assertTrue(
            (local_env_home / "meta" / project_id).is_dir(),
            "Expected project metadata in local env BAKE_HOME",
        )
        self.assertFalse(
            (override_home / "meta" / project_id).exists(),
            "Preset BAKE_HOME should be ignored when --local-env is set",
        )
        self.assertFalse(
            (override_home / "build" / project_id).exists(),
            "Preset BAKE_HOME should not receive local-env build output",
        )

    def test_local_env_named_variants_do_not_interfere(self) -> None:
        workspace = self.repo_root / "test" / "projects" / "ws"
        target = "apps/hello"
        project_id = "ws.apps.hello"
        stamp = int(time.time() * 1_000_000)
        env_a = f"named_a_{stamp}"
        env_b = f"named_b_{stamp}"
        local_env_root = workspace / ".bake" / "local_env"
        env_a_home = local_env_root / env_a
        env_b_home = local_env_root / env_b
        env_a_build_root = env_a_home / "build" / project_id
        env_b_build_root = env_b_home / "build" / project_id

        if env_a_home.exists():
            shutil.rmtree(env_a_home)
        if env_b_home.exists():
            shutil.rmtree(env_b_home)

        self.bake([f"--local-env={env_a}", "build", target], cwd=workspace)
        self.bake([f"--local-env={env_b}", "build", target], cwd=workspace)

        self.assertTrue(env_a_build_root.is_dir(), f"Expected named local env build root: {env_a_build_root}")
        self.assertTrue(env_b_build_root.is_dir(), f"Expected named local env build root: {env_b_build_root}")
        self.assertTrue(
            (env_a_home / "meta" / project_id).is_dir(),
            f"Expected project metadata in named local env: {env_a_home / 'meta' / project_id}",
        )
        self.assertTrue(
            (env_b_home / "meta" / project_id).is_dir(),
            f"Expected project metadata in named local env: {env_b_home / 'meta' / project_id}",
        )

        self.bake([f"--local-env={env_a}", "clean", target], cwd=workspace)

        self.assertFalse(
            env_a_build_root.exists(),
            f"Cleaning one named local env should not leave build output behind: {env_a_build_root}",
        )
        self.assertTrue(
            env_b_build_root.is_dir(),
            f"Cleaning one named local env should not remove the other: {env_b_build_root}",
        )

        output = self.strip_ansi(self.bake([f"--local-env={env_b}", "run", target], cwd=workspace))
        self.assertIn("hello 42", output)

    def test_local_env_rejects_invalid_and_reserved_names(self) -> None:
        workspace = self.repo_root / "test" / "projects" / "ws"
        target = "apps/hello"
        local_env_root = workspace / ".bake" / "local_env"

        for name in ("a b", "a/b", "..", "a:b"):
            output = self.strip_ansi(
                self.bake_expect_failure([f"--local-env={name}", "build", target], cwd=workspace))
            self.assertIn("invalid --local-env name", output, name)

        for name in ("build", "meta", "include", "test", "bin", "lib", "arm64-Darwin"):
            output = self.strip_ansi(
                self.bake_expect_failure([f"--local-env={name}", "build", target], cwd=workspace))
            self.assertIn("reserved --local-env name", output, name)
            self.assertFalse(
                (local_env_root / name / "build").exists(),
                f"Reserved name should not create an environment: {name}",
            )

    def test_local_env_names_share_bundle_sources(self) -> None:
        _, pkg_dir, _ = self.write_bundle_consumer_workspace("bundle_named_env")
        stamp = int(time.time() * 1_000_000)
        env_a = f"share_a_{stamp}"
        env_b = f"share_b_{stamp}"
        project_id = json.loads((pkg_dir / "project.json").read_text())["id"]
        bundle_id = next(iter(json.loads((pkg_dir / "project.json").read_text())["bundle"]))

        first = self.strip_ansi(self.bake([f"--local-env={env_a}", "build", "."], cwd=pkg_dir))
        second = self.strip_ansi(self.bake([f"--local-env={env_b}", "build", "."], cwd=pkg_dir))

        self.assertIn(f"fetching {bundle_id}", first)
        self.assertNotIn(
            f"fetching {bundle_id}",
            second,
            "A second named environment must reuse the bundle sources of the first",
        )

        bundle_src = pkg_dir / ".bake" / "bundles" / bundle_id / "default" / "src"
        self.assertTrue(bundle_src.is_dir(), f"Expected shared bundle source dir: {bundle_src}")
        self.assertFalse(
            (pkg_dir / ".bake" / "local_env" / env_a / "bundles").exists(),
            "Bundles must not be duplicated per named local environment",
        )

        for name in (env_a, env_b):
            build_root = pkg_dir / ".bake" / "local_env" / name / "build" / project_id
            self.assertTrue(build_root.is_dir(), f"Expected per-name build root: {build_root}")

    def test_local_env_concurrent_named_builds_share_bundle(self) -> None:
        _, pkg_dir, _ = self.write_bundle_consumer_workspace("bundle_concurrent_env")
        stamp = int(time.time() * 1_000_000)
        names = [f"conc_a_{stamp}", f"conc_b_{stamp}"]
        project_id = json.loads((pkg_dir / "project.json").read_text())["id"]
        bundle_id = next(iter(json.loads((pkg_dir / "project.json").read_text())["bundle"]))

        procs = [
            subprocess.Popen(
                [str(self.bake_bin), f"--local-env={name}", "build", "."],
                cwd=str(pkg_dir),
                env=self.env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            for name in names
        ]

        outputs = []
        for proc in procs:
            out, _ = proc.communicate(timeout=600)
            outputs.append((proc.returncode, self.strip_ansi(out or "")))

        for name, (code, out) in zip(names, outputs):
            self.assertEqual(code, 0, f"Concurrent build for {name} failed:\n{out}")

        fetches = sum(f"fetching {bundle_id}" in out for _, out in outputs)
        self.assertEqual(fetches, 1, "Concurrent named builds must clone the bundle exactly once")

        bundle_root = pkg_dir / ".bake" / "bundles" / bundle_id / "default"
        self.assertFalse(
            (bundle_root / ".lock").exists(),
            "Bundle lock must be released after a build",
        )

        artefacts = []
        for name in names:
            build_root = pkg_dir / ".bake" / "local_env" / name / "build" / project_id
            self.assertTrue(build_root.is_dir(), f"Expected per-name build root: {build_root}")
            found = list(build_root.glob("*/*"))
            self.assertTrue(found, f"Expected build output in {build_root}")
            artefacts.append({p.name for p in found})

        self.assertEqual(artefacts[0], artefacts[1])

    def ps_env(self, stamp: int) -> tuple[dict[str, str], Path]:
        ps_dir = self.repo_root / "test" / "tmp" / f"ps_registry_{stamp}"
        ps_dir.mkdir(parents=True, exist_ok=True)
        self.addCleanup(shutil.rmtree, ps_dir, ignore_errors=True)
        env = self.env.copy()
        env["BAKE3_PS_DIR"] = str(ps_dir)
        return env, ps_dir

    def ps_entries(self, env: dict[str, str], args: list[str] | None = None) -> list[dict]:
        output = self.strip_ansi(self.bake(["ps", "--json", *(args or [])], env=env))
        return json.loads(output)

    def ps_wait_for(
        self,
        env: dict[str, str],
        predicate,
        timeout: float = 60.0,
    ) -> list[dict]:
        deadline = time.time() + timeout
        entries = []
        while time.time() < deadline:
            entries = self.ps_entries(env)
            matched = [entry for entry in entries if predicate(entry)]
            if matched:
                return matched
            time.sleep(0.2)
        return []

    def ps_wait_until_gone(self, env: dict[str, str], pid: int, timeout: float = 30.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not any(entry["pid"] == pid for entry in self.ps_entries(env)):
                return True
            time.sleep(0.2)
        return False

    def write_forever_app(self, name: str) -> tuple[Path, str]:
        return self.write_simple_app_project(
            name,
            "#include <stdio.h>\n"
            "#if defined(_WIN32)\n"
            "#include <windows.h>\n"
            "#define bake_test_sleep() Sleep(200)\n"
            "#else\n"
            "#include <unistd.h>\n"
            "#define bake_test_sleep() usleep(200 * 1000)\n"
            "#endif\n"
            "int main(void) {\n"
            '    printf("forever app running\\n");\n'
            "    fflush(stdout);\n"
            "    for (;;) {\n"
            "        bake_test_sleep();\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
        )

    def test_ps_lists_and_kills_a_process_started_by_bake_run(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir, app_id = self.write_forever_app("ps_run")
        env_name = f"ps_run_{stamp}"
        env, _ = self.ps_env(stamp)

        self.bake([f"--local-env={env_name}", "--cfg", "release", "build", "."],
            cwd=project_dir, env=env)

        proc = subprocess.Popen(
            [str(self.bake_bin), f"--local-env={env_name}", "--cfg", "release", "run", "."],
            cwd=str(project_dir),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

        def stop_group() -> None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError, OSError):
                proc.kill()

        self.addCleanup(proc.stdout.close)
        self.addCleanup(proc.wait)
        self.addCleanup(stop_group)

        matched = self.ps_wait_for(env, lambda entry: entry["project"] == app_id)
        self.assertTrue(matched, f"bake ps did not list {app_id}")
        self.assertEqual(len(matched), 1, matched)

        entry = matched[0]
        self.assertEqual(entry["env"], f"local:{env_name}")
        self.assertEqual(entry["env_kind"], "local")
        self.assertEqual(entry["env_name"], env_name)
        self.assertEqual(entry["cfg"], "release")
        self.assertEqual(entry["kind"], "run")
        self.assertEqual(entry["source"], "registry")
        self.assertEqual(entry["state"], "running")
        self.assertGreaterEqual(entry["elapsed_sec"], 0)
        self.assertIn(app_id, entry["cmd"])
        self.assertEqual(entry["parent_pid"], proc.pid)

        table = self.strip_ansi(self.bake(["ps"], env=env))
        self.assertIn(str(entry["pid"]), table)
        self.assertIn(f"local:{env_name}", table)
        self.assertIn("PID", table.splitlines()[0])

        self.bake(["ps", "--kill", env_name], env=env)
        self.assertTrue(
            self.ps_wait_until_gone(env, entry["pid"]),
            "killed process is still listed by bake ps",
        )
        proc.wait(timeout=60)

        output = self.strip_ansi(proc.stdout.read())
        self.assertIn(f"started {app_id}", output)
        self.assertIn("bake3 ps", output)

    def test_ps_detects_a_directly_started_local_env_binary(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir, app_id = self.write_forever_app("ps_exec")
        env_name = f"ps_exec_{stamp}"
        env, _ = self.ps_env(stamp)

        self.bake([f"--local-env={env_name}", "build", "."], cwd=project_dir, env=env)

        env_home = project_dir / ".bake" / "local_env" / env_name
        candidates = [
            path for path in env_home.rglob(f"{app_id}{EXE_SUFFIX}")
            if path.is_file() and os.access(path, os.X_OK)
        ]
        self.assertTrue(candidates, f"no executable found under {env_home}")

        proc = subprocess.Popen(
            [str(candidates[0])],
            cwd=str(project_dir),
            env=env,
            text=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)

        matched = self.ps_wait_for(env, lambda entry: entry["pid"] == proc.pid)
        if not matched and platform.system() == "Windows":
            self.skipTest("process table scan is POSIX only")

        self.assertTrue(matched, "bake ps did not report the directly started binary")
        entry = matched[0]
        self.assertEqual(entry["source"], "scan")
        self.assertEqual(entry["env"], f"local:{env_name}")
        self.assertEqual(entry["env_kind"], "local")
        self.assertEqual(entry["env_name"], env_name)
        self.assertEqual(entry["project"], app_id)
        self.assertEqual(entry["state"], "running")
        self.assertEqual(entry["workspace"], str(project_dir))

        self.bake(["ps", "--kill", str(entry["pid"])], env=env)
        self.assertTrue(
            self.ps_wait_until_gone(env, entry["pid"]),
            "killed process is still listed by bake ps",
        )

    def test_ps_labels_an_unnamed_local_environment(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir, app_id = self.write_forever_app("ps_unnamed")
        env, _ = self.ps_env(stamp)

        self.bake(["--local-env", "build", "."], cwd=project_dir, env=env)

        proc = subprocess.Popen(
            [str(self.bake_bin), "--local-env", "run", "."],
            cwd=str(project_dir),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

        def stop_group() -> None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError, OSError):
                proc.kill()

        self.addCleanup(proc.stdout.close)
        self.addCleanup(proc.wait)
        self.addCleanup(stop_group)

        matched = self.ps_wait_for(env, lambda entry: entry["project"] == app_id)
        self.assertTrue(matched, f"bake ps did not list {app_id}")

        entry = matched[0]
        self.assertEqual(entry["env"], "local")
        self.assertEqual(entry["env_kind"], "local")
        self.assertEqual(entry["env_name"], "-")

        def rows_for(output: str, pid: int) -> list[str]:
            return [
                line for line in output.splitlines()
                if line.split() and line.split()[0] == str(pid)
            ]

        table = self.strip_ansi(self.bake(["ps"], env=env))
        rows = rows_for(table, entry["pid"])
        self.assertEqual(len(rows), 1, table)
        self.assertIn("local", rows[0])
        self.assertNotIn("(unnamed)", table)
        self.assertIn("run 'bake3 ps --full' to see full paths", table)
        self.assertEqual(
            table.splitlines()[-1], "run 'bake3 ps --full' to see full paths")

        full = self.strip_ansi(self.bake(["ps", "--full"], env=env))
        self.assertNotIn("run 'bake3 ps --full' to see full paths", full)
        full_rows = rows_for(full, entry["pid"])
        self.assertEqual(len(full_rows), 1, full)
        self.assertIn(str(project_dir), full_rows[0])
        self.assertIn(entry["cmd"], full_rows[0])
        self.assertNotIn("...", full_rows[0])

        json_output = self.strip_ansi(self.bake(["ps", "--json"], env=env))
        self.assertNotIn("run 'bake3 ps --full' to see full paths", json_output)

        output = self.strip_ansi(
            self.bake_expect_failure(["ps", "--kill", "global"], env=env))
        self.assertIn("no bake process matches 'global'", output)

        self.bake(["ps", "--kill", str(entry["pid"])], env=env)
        self.assertTrue(
            self.ps_wait_until_gone(env, entry["pid"]),
            "killed process is still listed by bake ps",
        )
        proc.wait(timeout=60)

        started = self.strip_ansi(proc.stdout.read())
        self.assertIn("env local)", started)

    def test_ps_kill_rejects_unknown_targets(self) -> None:
        stamp = int(time.time() * 1_000_000)
        env, _ = self.ps_env(stamp)
        output = self.strip_ansi(
            self.bake_expect_failure(["ps", "--kill", "2147483646"], env=env))
        self.assertIn("no bake process matches", output)

    def test_ps_options_are_rejected_for_other_commands(self) -> None:
        output = self.strip_ansi(self.bake_expect_failure(["--json", "list"]))
        self.assertIn("can only be used with the ps command", output)

        output = self.strip_ansi(self.bake_expect_failure(["--full", "list"]))
        self.assertIn("can only be used with the ps command", output)

    def test_local_env_dependency_isolation_between_workspaces(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"local_env_isolation_{stamp}"
        ws_a = root / "ws_a"
        ws_b = root / "ws_b"

        ws_a_src = ws_a / "src"
        ws_a_inc = ws_a / "include"
        ws_a_src.mkdir(parents=True, exist_ok=True)
        ws_a_inc.mkdir(parents=True, exist_ok=True)

        (ws_a / "project.json").write_text(
            "{\n"
            "    \"id\": \"dep.isolated.lib\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (ws_a_inc / "dep_isolated_lib.h").write_text(
            "#ifndef DEP_ISOLATED_LIB_H\n"
            "#define DEP_ISOLATED_LIB_H\n"
            "\n"
            "#include \"dep-isolated-lib/bake_config.h\"\n"
            "\n"
            "DEP_ISOLATED_LIB_API\n"
            "int dep_isolated_lib_value(void);\n"
            "\n"
            "#endif\n"
        )
        (ws_a_src / "main.c").write_text(
            "#include <dep_isolated_lib.h>\n"
            "\n"
            "int dep_isolated_lib_value(void) {\n"
            "    return 42;\n"
            "}\n"
        )

        ws_b_src = ws_b / "src"
        ws_b_inc = ws_b / "include"
        ws_b_src.mkdir(parents=True, exist_ok=True)
        ws_b_inc.mkdir(parents=True, exist_ok=True)

        (ws_b / "project.json").write_text(
            "{\n"
            "    \"id\": \"app.isolated.consumer\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": {\n"
            "        \"use\": [\"dep.isolated.lib\"]\n"
            "    }\n"
            "}\n"
        )
        (ws_b_inc / "app_isolated_consumer.h").write_text(
            "#ifndef APP_ISOLATED_CONSUMER_H\n"
            "#define APP_ISOLATED_CONSUMER_H\n"
            "\n"
            "#include \"app-isolated-consumer/bake_config.h\"\n"
            "\n"
            "#endif\n"
        )
        (ws_b_src / "main.c").write_text(
            "#include <app_isolated_consumer.h>\n"
            "\n"
            "int main(void) {\n"
            "    return dep_isolated_lib_value() == 42 ? 0 : 1;\n"
            "}\n"
        )

        self.bake(["--local-env", "build", "."], cwd=ws_a)
        fail_output = self.strip_ansi(
            self.bake_expect_failure(["--local-env", "build", "."], cwd=ws_b)
        )

        self.assertIn("unresolved dependency: dep.isolated.lib", fail_output)
        self.assertTrue(
            (ws_a / ".bake" / "local_env" / "meta" / "dep.isolated.lib").is_dir(),
            "Workspace A should contain dependency in its local env",
        )
        self.assertFalse(
            (ws_b / ".bake" / "local_env" / "meta" / "dep.isolated.lib").exists(),
            "Workspace B should not be able to resolve dependency from workspace A",
        )

    def test_local_env_initializes_test_harness_templates(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.localenv.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"local_env_test_project_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"local_env_test_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        local_env_home = self.repo_root / ".bake" / "local_env"
        if local_env_home.exists():
            shutil.rmtree(local_env_home)

        output = self.strip_ansi(self.bake(["--local-env", "run", str(project_dir)]))
        self.assertIn("PASS:", output)

        test_templates = local_env_home / "test"
        self.assertTrue(test_templates.is_dir(), f"Expected local test template dir: {test_templates}")
        self.assertTrue((test_templates / "bake_test.h").is_file())
        self.assertTrue((test_templates / "bake_test.c").is_file())
        self.assertTrue((test_templates / "bake_test_runtime.h").is_file())
        self.assertTrue((test_templates / "bake_test_runtime.c").is_file())

    def test_local_env_initializes_templates_from_global_bake_home(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.localenv.global.{stamp}"
        workspace = self.repo_root / "test" / "tmp" / f"local_env_global_home_{stamp}"
        project_dir = workspace / "project"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"local_env_global_home_test\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        seed_home = workspace / "seed_bake_home"
        seed_test = seed_home / "test"
        source_templates = self.repo_root / "templates" / "test_harness"
        shutil.copytree(source_templates, seed_test)

        env = self.env.copy()
        env["BAKE_HOME"] = str(seed_home)
        env["BAKE_LOCAL_ENV"] = "0"
        env.pop("BAKE_GLOBAL_HOME", None)

        output = self.strip_ansi(self.bake(["--local-env", "run", "."], cwd=project_dir, env=env))
        self.assertIn("PASS:", output)

        local_templates = project_dir / ".bake" / "local_env" / "test"
        self.assertTrue(local_templates.is_dir(), f"Expected local template dir: {local_templates}")
        self.assertTrue((local_templates / "bake_test.h").is_file())
        self.assertTrue((local_templates / "bake_test.c").is_file())
        self.assertTrue((local_templates / "bake_test_runtime.h").is_file())
        self.assertTrue((local_templates / "bake_test_runtime.c").is_file())

    def test_build_nonexistent_target_fails(self) -> None:
        output = self.strip_ansi(
            self.bake_expect_failure(["build", "test/projects/c/does_not_exist"])
        )
        self.assertIn("not found", output.lower())

    def test_build_invalid_project_json_fails(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir = self.repo_root / "test" / "tmp" / f"invalid_json_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text("{invalid json")

        (src_dir / "main.c").write_text(
            "int main(void) {\n"
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(
            self.bake_expect_failure(["build", str(project_dir)])
        )
        lower = output.lower()
        has_parse_message = (
            "failed to parse" in lower
            or "parse error" in lower
            or "json" in lower
            or "syntax" in lower
        )
        self.assertTrue(
            has_parse_message,
            f"Expected JSON parse error message, got:\n{output}",
        )
        self.assertIn(
            "project.json",
            output,
            f"Expected project.json reference in error, got:\n{output}",
        )

    def test_build_missing_dependency_fails(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir = self.repo_root / "test" / "tmp" / f"missing_dep_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"tmp.missing_dep_test.{stamp}\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": {\n"
            "        \"use\": [\"nonexistent.package.xyz\"]\n"
            "    }\n"
            "}\n"
        )

        (src_dir / "main.c").write_text(
            "int main(void) {\n"
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(
            self.bake_expect_failure(["build", str(project_dir)])
        )
        has_useful_message = (
            "unresolved dependency" in output.lower()
            or "nonexistent.package.xyz" in output
        )
        self.assertTrue(
            has_useful_message,
            f"Expected error about unresolved dependency or missing package name, got:\n{output}",
        )

    def test_run_nonexistent_target_fails(self) -> None:
        output = self.strip_ansi(
            self.bake_expect_failure(["run", "test/projects/c/does_not_exist"])
        )
        lower = output.lower()
        self.assertIn(
            "not found",
            lower,
            f"Expected 'not found' in error output, got:\n{output}",
        )
        self.assertIn(
            "does_not_exist",
            output,
            f"Expected missing target name in error output, got:\n{output}",
        )

    def test_circular_dependency_does_not_crash(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"circular_dep_{stamp}"
        proj_a = root / "a"
        proj_b = root / "b"
        for d in (proj_a / "src", proj_b / "src"):
            d.mkdir(parents=True, exist_ok=True)

        (proj_a / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.cycle.a.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            f"        \"use\": [\"tmp.cycle.b.{stamp}\"]\n"
            "    }\n"
            "}\n"
        )
        (proj_a / "src" / "a.c").write_text("int a_value(void){return 1;}\n")

        (proj_b / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.cycle.b.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            f"        \"use\": [\"tmp.cycle.a.{stamp}\"]\n"
            "    }\n"
            "}\n"
        )
        (proj_b / "src" / "b.c").write_text("int b_value(void){return 2;}\n")

        proc = subprocess.run(
            [str(self.bake_bin), "build", str(root)],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(
            proc.returncode,
            -11,
            "bake crashed (segfault) on circular dependency",
        )
        self.assertGreaterEqual(
            proc.returncode,
            0,
            f"bake terminated by signal on circular dependency: rc={proc.returncode}",
        )

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_removed_key_aliases_have_no_effect(self) -> None:
        """The dropped alias spellings are inert, not silently honored."""
        stamp = int(time.time() * 1_000_000)
        app_id = f"alias_gone_{stamp}"
        root = self.repo_root / "test" / "tmp" / f"alias_gone_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "{app_id}",\n'
            '    "type": "application",\n'
            '    "value": {\n'
            '        "artefact": "renamed_by_alias"\n'
            "    },\n"
            '    "lang.c": {\n'
            '        "libs": ["m"],\n'
            '        "libpaths": ["/nonexistent"]\n'
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text("int main(void) { return 0; }\n")

        output = self.strip_ansi(self.bake(["--trace", "build", str(root)]))

        self.assertNotIn(" -lm", output, "'libs' must no longer feed the linker")
        self.assertNotIn("/nonexistent", output, "'libpaths' must no longer feed the linker")

        triplet_dir = root / ".bake" / f"{self.host_arch()}-{platform.system()}-debug"
        self.assertTrue(
            (triplet_dir / f"{app_id}{EXE_SUFFIX}").is_file(),
            f"artefact should keep the project id name in {triplet_dir}")
        self.assertFalse(
            (triplet_dir / f"renamed_by_alias{EXE_SUFFIX}").exists(),
            "'artefact' must no longer rename the output")

    def test_similar_keys_suggest_the_supported_spelling(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"suggest_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "suggest_{stamp}",\n'
            '    "type": "application",\n'
            '    "value": {\n'
            '        "libs": [],\n'
            '        "use_private": []\n'
            "    },\n"
            '    "lang.c": {\n'
            '        "includes": []\n'
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text("int main(void) { return 0; }\n")

        output = self.strip_ansi(self.bake(["build", str(root)]))

        self.assertIn("unknown project key 'libs', did you mean 'lib'?", output)
        self.assertIn(
            "unknown project key 'use_private', did you mean 'use-private'?", output)
        self.assertIn(
            "unknown language config key 'includes', did you mean 'include'?", output)

    def _make_lint_project(self, name: str, action: str | None) -> tuple[Path, Path]:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        (root / "src").mkdir(parents=True)
        log = root / "lint.log"
        (root / "lint.py").write_text(
            "import sys\n"
            "path, action = sys.argv[1], sys.argv[2]\n"
            "with open('lint.log', 'a') as f:\n"
            "    f.write(path + ' ' + action + '\\n')\n"
            "text = open(path).read()\n"
            "if action == 'autofix':\n"
            "    text = text.replace('LINT_BAD', 'LINT_OK')\n"
            "    open(path, 'w').write(text)\n"
            "failed = 'LINT_BAD' in text or 'LINT_WARN' in text\n"
            "if failed:\n"
            "    print('src/x.c:1: found a lint marker')\n"
            "sys.exit(1 if failed else 0)\n"
        )
        lint = {"command": f'"{Path(sys.executable).as_posix()}" lint.py'}
        if action is not None:
            lint["action"] = action
        (root / "project.json").write_text(json.dumps({
            "id": f"{name}_{stamp}",
            "type": "application",
            "value": {"public": False},
            "lint": lint,
        }, indent=4))
        (root / "src" / "main.c").write_text("int main(void) { return 0; }\n")
        return root, log

    def test_lint_error_fails_build(self) -> None:
        root, log = self._make_lint_project("lint_error", None)
        (root / "src" / "bad.c").write_text("int LINT_BAD = 0;\n")

        output = self.strip_ansi(self.bake_expect_failure(["build", str(root)]))
        bad = (root / "src" / "bad.c").resolve()
        self.assertIn("lint failed for src/bad.c", output)
        self.assertIn("lint: src/x.c:1: found a lint marker", output)
        self.assertIn(f"{bad} error", log.read_text().splitlines())

        (root / "src" / "bad.c").write_text("int LINT_OK = 0;\n")
        log.unlink()
        self.bake(["build", str(root)])
        lines = log.read_text().splitlines()
        self.assertIn(f"{bad} error", lines)
        self.assertIn(f"{(root / 'src' / 'main.c').resolve()} error", lines)

        log.unlink()
        self.bake(["build", str(root)])
        self.assertFalse(log.exists())

    def test_lint_log_reports_and_continues(self) -> None:
        root, log = self._make_lint_project("lint_log", "log")
        (root / "src" / "warn.c").write_text("int LINT_WARN = 0;\n")

        output = self.strip_ansi(self.bake(["build", str(root)]))
        self.assertIn("lint reported problems in src/warn.c", output)
        warn = (root / "src" / "warn.c").resolve()
        self.assertIn(f"{warn} log", log.read_text().splitlines())

    def test_lint_autofix_rewrites_before_compile(self) -> None:
        root, log = self._make_lint_project("lint_autofix", "autofix")
        (root / "src" / "fix.c").write_text("int LINT_BAD = 0;\n")

        self.bake(["build", str(root)])
        self.assertEqual((root / "src" / "fix.c").read_text(), "int LINT_OK = 0;\n")
        fix = (root / "src" / "fix.c").resolve()
        self.assertIn(f"{fix} autofix", log.read_text().splitlines())

    def test_fix_lint_option_overrides_action(self) -> None:
        root, log = self._make_lint_project("lint_fix_option", "error")
        (root / "src" / "fix.c").write_text("int LINT_BAD = 0;\n")

        self.bake(["build", str(root), "--fix-lint"])
        self.assertEqual((root / "src" / "fix.c").read_text(), "int LINT_OK = 0;\n")
        fix = (root / "src" / "fix.c").resolve()
        self.assertIn(f"{fix} autofix", log.read_text().splitlines())

    def test_lint_invalid_action_is_rejected(self) -> None:
        root, _ = self._make_lint_project("lint_invalid", "fix")

        output = self.strip_ansi(self.bake_expect_failure(["build", str(root)]))
        self.assertIn("invalid lint action 'fix'", output)

    def test_lint_skips_generated_test_main(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"lint_harness_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "lint.py").write_text(
            "import sys\n"
            "with open('lint.log', 'a') as f:\n"
            "    f.write(sys.argv[1] + '\\n')\n"
        )
        (root / "project.json").write_text(json.dumps({
            "id": f"lint_harness_{stamp}",
            "type": "application",
            "value": {"public": False},
            "lint": {"command": f'"{Path(sys.executable).as_posix()}" lint.py'},
            "test": {"testsuites": [{"id": "Suite", "testcases": ["case"]}]},
        }, indent=4))

        self.bake(["build", str(root)])
        linted = [Path(l).name for l in (root / "lint.log").read_text().splitlines()]
        self.assertIn("Suite.c", linted)
        self.assertNotIn("main.c", linted)

    def test_similar_bundle_and_amalgamate_keys_are_reported(self) -> None:
        """A misspelled bundle key is ignored, so the build fails on the
        default cmake path -- which is exactly what the suggestion warns
        about."""
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"suggest_bundle_{stamp}"
        repo = root / "hdr_repo"
        (repo / "inc").mkdir(parents=True)
        (repo / "inc" / "hdr.h").write_text("#define HDR_VALUE 1\n")
        self._git_init_repo(repo)

        pkg = root / "pkg"
        (pkg / "src").mkdir(parents=True)
        (pkg / "project.json").write_text(
            "{\n"
            f'    "id": "suggest_pkg_{stamp}",\n'
            '    "type": "package",\n'
            '    "bundle": {\n'
            f'        "hdr": {{ "repository": "{repo.as_posix()}", '
            '"header_only": true, "cmake_args": [] }\n'
            "    },\n"
            '    "value": {\n'
            '        "amalgamate": { "path": "distr", "disable_flags": [] }\n'
            "    }\n"
            "}\n"
        )
        (pkg / "src" / "a.c").write_text("int suggest_pkg(void) { return 1; }\n")

        output = self.strip_ansi(self.bake_expect_failure(["build", str(pkg)]))

        self.assertIn(
            "unknown bundle key 'header_only', did you mean 'header-only'?", output)
        self.assertIn(
            "unknown bundle key 'cmake_args', did you mean 'cmake-args'?", output)
        self.assertIn(
            "unknown amalgamate key 'disable_flags', did you mean 'disable-flags'?",
            output)

    def test_unrelated_unknown_keys_are_not_reported(self) -> None:
        """Project metadata bake does not consume must stay silent.

        Warning on every unrecognized key would bury the useful suggestions
        under one line per description/author field in the tree.
        """
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"quiet_keys_{stamp}"
        (root / "src").mkdir(parents=True)
        (root / "project.json").write_text(
            "{\n"
            f'    "id": "quiet_keys_{stamp}",\n'
            '    "type": "application",\n'
            '    "value": {\n'
            '        "author": "someone",\n'
            '        "description": "a project",\n'
            '        "license": "MIT",\n'
            '        "keywords": ["build"]\n'
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text("int main(void) { return 0; }\n")

        output = self.strip_ansi(self.bake(["build", str(root)]))

        self.assertNotIn("did you mean", output)

    def test_duplicate_project_id_reports_error(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"dup_id_{stamp}"
        proj_a = root / "a"
        proj_b = root / "b"
        for d in (proj_a / "src", proj_b / "src"):
            d.mkdir(parents=True, exist_ok=True)

        proj_id = f"tmp.dup.id.{stamp}"
        for proj in (proj_a, proj_b):
            (proj / "project.json").write_text(
                "{\n"
                f"    \"id\": \"{proj_id}\",\n"
                "    \"type\": \"package\"\n"
                "}\n"
            )
            (proj / "src" / "src.c").write_text("int v(void){return 0;}\n")

        proc = subprocess.run(
            [str(self.bake_bin), "build", str(root)],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        self.assertGreaterEqual(
            proc.returncode,
            0,
            f"bake terminated by signal on duplicate id: rc={proc.returncode}\n{output}",
        )
        self.assertIn(
            "duplicate",
            output.lower(),
            f"expected duplicate-id error, got:\n{output}",
        )

    def test_unknown_conditional_kind_warns(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"unknown_cond_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.unknown.cond.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            "        \"${oss linux}\": {\n"
            "            \"cflags\": [\"-DSHOULD_NOT_APPLY\"]\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (root / "src" / "x.c").write_text("int x_value(void){return 0;}\n")
        proc = subprocess.run(
            [str(self.bake_bin), "build", str(root)],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        self.assertIn(
            "unknown conditional key",
            output,
            f"expected unknown-conditional warning in output:\n{output}",
        )

    @staticmethod
    def host_arch() -> str:
        machine = platform.machine().lower()
        if machine in {"arm64", "aarch64"}:
            return "arm64"
        if machine in {"x86_64", "amd64"}:
            return "x64"
        if machine in {"i386", "i686", "x86"}:
            return "x86"
        if machine.startswith("arm"):
            return "arm"
        return "unknown"

    def write_simple_app_project(
        self, name: str, main_c: str, lang_c: str | None = None
    ) -> tuple[Path, str]:
        stamp = int(time.time() * 1_000_000)
        app_id = f"{name}_{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True)

        lang_c_section = f',\n    "lang.c": {lang_c}' if lang_c else ""
        (project_dir / "project.json").write_text(
            "{\n"
            f'    "id": "{app_id}",\n'
            f'    "type": "application"{lang_c_section}\n'
            "}\n"
        )
        (src_dir / "main.c").write_text(main_c)
        return project_dir, app_id

    def test_cfg_release_builds_separate_triplet(self) -> None:
        project_dir, app_id = self.write_simple_app_project(
            "cfg_modes",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("cfg_modes ok\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        self.bake(["--cfg", "release", "build", str(project_dir)])

        triplet_base = f"{self.host_arch()}-{platform.system()}"
        release_dir = project_dir / ".bake" / f"{triplet_base}-release"
        debug_dir = project_dir / ".bake" / f"{triplet_base}-debug"
        release_artefact = release_dir / f"{app_id}{EXE_SUFFIX}"

        self.assertTrue(
            release_artefact.is_file(),
            f"Expected release artefact at {release_artefact}",
        )
        self.assertFalse(
            debug_dir.exists(),
            f"Release build should not create debug triplet dir: {debug_dir}",
        )

        output = self.run_cmd([str(release_artefact)])
        self.assertIn("cfg_modes ok", output)

        release_mtime = release_artefact.stat().st_mtime_ns

        self.bake(["build", str(project_dir)])

        debug_artefact = debug_dir / f"{app_id}{EXE_SUFFIX}"
        self.assertTrue(
            debug_artefact.is_file(),
            f"Expected debug artefact at {debug_artefact}",
        )
        self.assertTrue(
            release_artefact.is_file(),
            f"Debug build clobbered release artefact at {release_artefact}",
        )
        self.assertEqual(
            release_artefact.stat().st_mtime_ns,
            release_mtime,
            "Debug build modified the release artefact",
        )

        self.assertTrue((release_dir / "obj").is_dir(), f"Expected obj dir in {release_dir}")
        self.assertTrue((debug_dir / "obj").is_dir(), f"Expected obj dir in {debug_dir}")

        release_fingerprint = release_dir / ".bake_cmd"
        debug_fingerprint = debug_dir / ".bake_cmd"
        self.assertTrue(release_fingerprint.is_file(), f"Expected fingerprint at {release_fingerprint}")
        self.assertTrue(debug_fingerprint.is_file(), f"Expected fingerprint at {debug_fingerprint}")
        self.assertNotEqual(
            release_fingerprint.read_text(),
            debug_fingerprint.read_text(),
            "Expected release and debug builds to have distinct command fingerprints",
        )

    def test_help_lists_commands_and_options(self) -> None:
        for flag in ("-h", "--help"):
            output = self.strip_ansi(self.bake([flag]))
            self.assertIn("Usage: bake", output, flag)
            self.assertIn("build [target]", output, flag)
            self.assertIn("--local-env", output, flag)
            self.assertIn("-j <count>", output, flag)

    def test_cleanup_removes_projects_whose_source_is_gone(self) -> None:
        stamp = int(time.time() * 1_000_000)
        pkg_id = f"cleanup_pkg_{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"cleanup_{stamp}"
        (project_dir / "src").mkdir(parents=True)
        (project_dir / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package"}}\n'
        )
        (project_dir / "src" / "a.c").write_text("int cleanup_probe(void) { return 1; }\n")

        self.bake(["build", str(project_dir)])
        self.assertIn(pkg_id, self.list_state().package_names)

        self._rm_tree(project_dir)

        output = self.strip_ansi(self.bake(["cleanup"]))
        self.assertIn("removed 1 stale project", output)
        self.assertNotIn(pkg_id, self.list_state().package_names)

    def test_cleanup_keeps_projects_whose_source_still_exists(self) -> None:
        stamp = int(time.time() * 1_000_000)
        pkg_id = f"cleanup_keep_{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"cleanup_keep_{stamp}"
        (project_dir / "src").mkdir(parents=True)
        (project_dir / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package"}}\n'
        )
        (project_dir / "src" / "a.c").write_text("int cleanup_keep(void) { return 1; }\n")

        self.bake(["build", str(project_dir)])
        output = self.strip_ansi(self.bake(["cleanup"]))

        self.assertIn("removed 0 stale project", output)
        self.assertIn(pkg_id, self.list_state().package_names)

    @unittest.skipIf(platform.system() == "Windows", "overrides gcc-style compilers; bake defaults to MSVC on Windows")
    def test_cc_option_overrides_c_compiler(self) -> None:
        compiler = shutil.which("clang") or shutil.which("gcc")
        if not compiler:
            self.skipTest("no alternative C compiler on PATH")
        name = Path(compiler).name

        project_dir, _ = self.write_simple_app_project(
            "cc_override", "int main(void) { return 0; }\n"
        )
        output = self.strip_ansi(
            self.bake(["--trace", "--cc", name, "build", str(project_dir)])
        )

        self.assertIn(f"{name} -c", output)

    @unittest.skipIf(platform.system() == "Windows", "overrides gcc-style compilers; bake defaults to MSVC on Windows")
    def test_cxx_option_overrides_cpp_compiler(self) -> None:
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("no alternative C++ compiler on PATH")
        name = Path(compiler).name

        stamp = int(time.time() * 1_000_000)
        project_dir = self.repo_root / "test" / "tmp" / f"cxx_override_{stamp}"
        (project_dir / "src").mkdir(parents=True)
        (project_dir / "project.json").write_text(
            f'{{"id": "cxx_override_{stamp}", "type": "application", '
            '"value": {"language": "cpp"}}\n'
        )
        (project_dir / "src" / "main.cpp").write_text("int main() { return 0; }\n")

        output = self.strip_ansi(
            self.bake(["--trace", "--cxx", name, "build", str(project_dir)])
        )

        self.assertIn(f"{name} -c", output)

    def test_standalone_option_amalgamates_without_project_json_opt_in(self) -> None:
        """--standalone turns on amalgamated deps for a project that does not
        request it in project.json."""
        self.bake(["build", "test/projects/c/pkg_helloworld"])

        stamp = int(time.time() * 1_000_000)
        app_id = f"standalone_opt_{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"standalone_opt_{stamp}"
        (project_dir / "src").mkdir(parents=True)
        (project_dir / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            '"value": {"use": ["examples.c.pkg_helloworld"]}}\n'
        )
        (project_dir / "src" / "main.c").write_text(
            "#include <examples_c_pkg_helloworld.h>\n"
            "int main(void) { pkg_helloworld(); return 0; }\n"
        )

        deps_dir = project_dir / "deps"
        self.bake(["build", str(project_dir)])
        self.assertFalse(
            deps_dir.exists(),
            "a plain build must not amalgamate dependencies")

        output = self.strip_ansi(self.bake(["--standalone", "run", str(project_dir)]))
        self.assertIn("Hello world", output)
        self.assertTrue(
            (deps_dir / "examples_c_pkg_helloworld.c").is_file(),
            f"--standalone did not amalgamate the dependency into {deps_dir}")

    def test_parallel_jobs_option_builds_every_source(self) -> None:
        stamp = int(time.time() * 1_000_000)
        app_id = f"jobs_{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"jobs_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True)
        (project_dir / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application"}}\n'
        )

        unit_count = 12
        for i in range(unit_count):
            (src_dir / f"unit{i}.c").write_text(f"int unit{i}(void) {{ return {i}; }}\n")
        calls = " + ".join(f"unit{i}()" for i in range(unit_count))
        externs = "".join(f"int unit{i}(void);\n" for i in range(unit_count))
        (src_dir / "main.c").write_text(
            "#include <stdio.h>\n"
            f"{externs}"
            "int main(void) {\n"
            f'    printf("sum=%d\\n", {calls});\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(self.bake(["-j", "4", "run", str(project_dir)]))
        self.assertIn(f"sum={sum(range(unit_count))}", output)

        obj_dir = (
            project_dir / ".bake" /
            f"{self.host_arch()}-{platform.system()}-debug" / "obj" / "src"
        )
        objects = sorted(
            p.name for p in obj_dir.glob(f"*{OBJ_SUFFIX}") if p.is_file())
        self.assertEqual(
            len(objects), unit_count + 1,
            f"expected {unit_count + 1} objects from the parallel build, got {objects}")

    def assert_cfg_mode_builds(self, name: str, mode: str) -> None:
        project_dir, app_id = self.write_simple_app_project(
            name,
            "#include <stdio.h>\n"
            "int main(void) {\n"
            f'    printf("{name} ok\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        self.bake(["--cfg", mode, "build", str(project_dir)])

        triplet = f"{self.host_arch()}-{platform.system()}-{mode}"
        artefact = project_dir / ".bake" / triplet / f"{app_id}{EXE_SUFFIX}"
        self.assertTrue(artefact.is_file(), f"Expected {mode} artefact at {artefact}")
        self.assertIn(f"{name} ok", self.run_cmd([str(artefact)]))

    def test_cfg_profile_builds_separate_triplet(self) -> None:
        self.assert_cfg_mode_builds("cfg_profile", "profile")

    @unittest.skipIf(platform.system() == "Windows", "mingw-w64 ships no asan/ubsan runtime")
    def test_cfg_sanitize_builds_separate_triplet(self) -> None:
        self.assert_cfg_mode_builds("cfg_sanitize", "sanitize")

    def test_conditional_os_and_cfg_blocks_apply(self) -> None:
        host_os = platform.system()
        other_os = "Linux" if host_os == "Darwin" else "Darwin"
        lang_c = (
            "{\n"
            f'        "${{os {host_os}}}": {{ "defines": ["HOST_OS_MATCHED"] }},\n'
            f'        "${{os {other_os}}}": {{ "defines": ["OTHER_OS_MATCHED"] }},\n'
            '        "${cfg debug}": { "defines": ["CFG_DEBUG_MATCHED"] },\n'
            '        "${cfg release}": { "defines": ["CFG_RELEASE_MATCHED"] }\n'
            "    }"
        )
        project_dir, _ = self.write_simple_app_project(
            "cond_match",
            "#ifndef HOST_OS_MATCHED\n"
            '#error "matching ${os} conditional was not applied"\n'
            "#endif\n"
            "#ifdef OTHER_OS_MATCHED\n"
            '#error "non-matching ${os} conditional was applied"\n'
            "#endif\n"
            "#ifndef CFG_DEBUG_MATCHED\n"
            '#error "matching ${cfg} conditional was not applied"\n'
            "#endif\n"
            "#ifdef CFG_RELEASE_MATCHED\n"
            '#error "non-matching ${cfg} conditional was applied"\n'
            "#endif\n"
            "int main(void) {\n"
            "    return 0;\n"
            "}\n",
            lang_c=lang_c,
        )

        self.bake(["build", str(project_dir)])

    @unittest.skipIf(platform.system() == "Windows", "run-prefix test uses a POSIX shell script")
    def test_run_prefix_wraps_executed_command(self) -> None:
        project_dir, app_id = self.write_simple_app_project(
            "run_prefix",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("run_prefix app output\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        marker = f"run_prefix_marker_{app_id}"
        script = project_dir / "prefix.sh"
        script.write_text(
            "#!/bin/sh\n"
            f"echo {marker}\n"
            'exec "$@"\n'
        )
        script.chmod(0o755)

        output = self.strip_ansi(
            self.bake(["run", str(project_dir), "--run-prefix", str(script)])
        )
        self.assertIn(marker, output)
        self.assertIn("run_prefix app output", output)

    def test_run_uses_project_dir_as_cwd(self) -> None:
        project_dir, _ = self.write_simple_app_project(
            "run_cwd",
            "#include <stdio.h>\n"
            "#if defined(_WIN32)\n"
            "#include <direct.h>\n"
            "#define getcwd _getcwd\n"
            "#else\n"
            "#include <unistd.h>\n"
            "#endif\n"
            "int main(void) {\n"
            "    char buf[4096];\n"
            "    if (!getcwd(buf, sizeof(buf))) {\n"
            "        return 1;\n"
            "    }\n"
            '    printf("run_cwd=%s\\n", buf);\n'
            "    return 0;\n"
            "}\n",
        )

        relative_target = project_dir.relative_to(self.repo_root)
        output = self.strip_ansi(self.bake(["run", str(relative_target)]))

        def norm(value: object) -> str:
            return str(value).replace("\\", "/")

        normalized = norm(output)
        expected = norm(Path(os.path.realpath(project_dir)))
        self.assertIn(f"run_cwd={expected}", normalized)
        self.assertNotIn(
            f"run_cwd={norm(Path(os.path.realpath(self.repo_root)))}\n",
            normalized)

    @staticmethod
    def emsdk_available() -> bool:
        if shutil.which("emcc"):
            return True
        candidates = [os.environ.get("EMSDK"), os.environ.get("EMSDK_DIR")]
        candidates.append(str(Path.home() / "GitHub" / "emsdk"))
        for candidate in candidates:
            if not candidate:
                continue
            root = Path(candidate)
            if (root / "emsdk_env.sh").is_file() and (
                root / "upstream" / "emscripten" / "emcc"
            ).is_file():
                return True
        return False

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_target_em_builds_wasm_artefacts(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, app_id = self.write_simple_app_project(
            "em_target",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        self.bake(["--target", "em", "build", str(project_dir)])

        triplet_dir = project_dir / ".bake" / "wasm32-Emscripten-debug"
        self.assertTrue(
            triplet_dir.is_dir(),
            f"Expected emscripten triplet dir at {triplet_dir}",
        )

        js = triplet_dir / f"{app_id}.js"
        wasm = triplet_dir / f"{app_id}.wasm"
        self.assertTrue(js.is_file(), f"Expected .js artefact at {js}")
        self.assertTrue(wasm.is_file(), f"Expected sibling .wasm at {wasm}")

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_target_em_project_ldflags_override_defaults(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, _ = self.write_simple_app_project(
            "em_override",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
            lang_c='{"${target em}": {"ldflags": ["-sMODULARIZE=0"]}}',
        )

        output = self.strip_ansi(
            self.bake(["--target", "em", "--trace", "build", str(project_dir)]))

        default_at = output.find("-s MODULARIZE=1")
        override_at = output.find("-sMODULARIZE=0")
        self.assertNotEqual(default_at, -1, "Expected bake to emit -s MODULARIZE=1")
        self.assertNotEqual(override_at, -1, "Expected project ldflag on the link")
        self.assertLess(
            default_at,
            override_at,
            "Project ldflags must be emitted after bake's emscripten defaults",
        )

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_target_em_run_serves_the_build_dir(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, app_id = self.write_simple_app_project(
            "em_serve",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
            lang_c='{"${target em}": {"shell": "etc/shell.html"}}',
        )
        shell = project_dir / "etc" / "shell.html"
        shell.parent.mkdir(parents=True, exist_ok=True)
        shell.write_text("<!doctype html><body>{{{ SCRIPT }}}</body>\n")

        env = self.env.copy()
        env["BROWSER"] = "true"  # webbrowser.open must not raise a window

        proc = subprocess.Popen(
            [str(self.bake_bin), "--target", "em", "--port", "8123",
             "run", str(project_dir)],
            cwd=str(self.repo_root),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

        def stop_server_group() -> None:
            # bake spawns the web server as a child, so terminating bake alone
            # leaves the port bound.
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                proc.terminate()

        self.addCleanup(proc.stdout.close)
        self.addCleanup(proc.wait)
        self.addCleanup(stop_server_group)

        url = None
        deadline = time.time() + 60
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            match = re.search(r"at (http://localhost:\d+/\S+)", self.strip_ansi(line))
            if match:
                url = match.group(1)
                break

        self.assertIsNotNone(url, "bake run did not report a served url")
        self.assertTrue(url.endswith(f"{app_id}.html"), url)
        self.assertIn(":8123/", url, "--port was not honoured")

        with urllib.request.urlopen(url, timeout=10) as response:
            self.assertEqual(response.status, 200)
            self.assertIn(f"{app_id}.js", response.read().decode())

        wasm_url = url[: -len(".html")] + ".wasm"
        with urllib.request.urlopen(wasm_url, timeout=10) as response:
            self.assertEqual(response.status, 200)

    def test_invalid_port_is_rejected(self) -> None:
        output = self.bake_expect_failure(["--port", "0", "build", "."])
        self.assertIn("invalid value for --port", self.strip_ansi(output))

        output = self.bake_expect_failure(["--port", "http", "build", "."])
        self.assertIn("invalid value for --port", self.strip_ansi(output))

        output = self.bake_expect_failure(["--port"])
        self.assertIn("missing value for --port", self.strip_ansi(output))

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_target_em_syncs_sibling_wasm_into_env(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, app_id = self.write_simple_app_project(
            "em_sync",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        self.bake(["--target", "em", "build", str(project_dir)])

        bin_dir = self.bake_home / "wasm32-Emscripten" / "debug" / "bin"
        self.addCleanup(shutil.rmtree, bin_dir / app_id, ignore_errors=True)
        js = bin_dir / f"{app_id}.js"
        wasm = bin_dir / f"{app_id}.wasm"
        self.addCleanup(js.unlink, missing_ok=True)
        self.addCleanup(wasm.unlink, missing_ok=True)
        self.assertTrue(js.is_file(), f"Expected .js in env at {js}")
        self.assertTrue(wasm.is_file(), f"Expected sibling .wasm in env at {wasm}")

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_target_em_shell_file_emits_html(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, app_id = self.write_simple_app_project(
            "em_shell",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
            lang_c='{"${target em}": {"shell": "etc/shell.html"}}',
        )
        shell = project_dir / "etc" / "shell.html"
        shell.parent.mkdir(parents=True, exist_ok=True)
        shell.write_text("<!doctype html><body>{{{ SCRIPT }}}</body>\n")

        output = self.strip_ansi(
            self.bake(["--target", "em", "--trace", "build", str(project_dir)]))
        self.assertIn("--shell-file", output)
        self.assertIn("shell.html", output)

        triplet_dir = project_dir / ".bake" / "wasm32-Emscripten-debug"
        html = triplet_dir / f"{app_id}.html"
        self.assertTrue(html.is_file(), f"Expected .html artefact at {html}")
        self.assertIn(f"{app_id}.js", html.read_text())
        self.assertTrue((triplet_dir / f"{app_id}.js").is_file())
        self.assertTrue((triplet_dir / f"{app_id}.wasm").is_file())

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_target_em_release_passes_optimization_to_link(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, app_id = self.write_simple_app_project(
            "em_release_opt",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        output = self.strip_ansi(self.bake(
            ["--target", "em", "--cfg", "release", "--trace", "build", str(project_dir)]))

        link_lines = [l for l in output.splitlines() if f"-o " in l and f"{app_id}.js" in l]
        self.assertTrue(link_lines, f"No link command found in:\n{output}")
        self.assertIn("-O3", link_lines[-1])

    def test_json_strlist_skips_nulls(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"json_null_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.json.null.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            "        \"use\": [null]\n"
            "    }\n"
            "}\n"
        )
        (root / "src" / "x.c").write_text("int x_value(void){return 0;}\n")
        proc = subprocess.run(
            [str(self.bake_bin), "info", str(root)],
            cwd=str(root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        text = self.strip_ansi(output)
        for line in text.splitlines():
            if line.startswith("use:") or line.lstrip().startswith("- "):
                self.assertNotIn(
                    "null",
                    line,
                    f"null entry leaked into use list:\n{text}",
                )

    def test_clean_nonexistent_target_succeeds(self) -> None:
        proc = subprocess.run(
            [str(self.bake_bin), "clean", "test/projects/c/does_not_exist"],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"clean of nonexistent target should succeed silently, "
            f"rc={proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
        )
        stderr_clean = self.strip_ansi(proc.stderr or "").strip()
        self.assertEqual(
            stderr_clean,
            "",
            f"clean of nonexistent target should not produce stderr, got:\n{proc.stderr}",
        )

    def test_build_bundle_via_cmake(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        project_dir = self.repo_root / "test" / "tests" / "app_bundle"
        bundle_repo = project_dir / "bundle_repo"
        bake_dir = project_dir / ".bake"

        def _force_writable(path: Path) -> None:
            try:
                path.chmod(stat.S_IWUSR | stat.S_IRUSR | stat.S_IXUSR)
            except OSError:
                pass

        def _rmtree(path: Path) -> None:
            def onerror(func, target, _exc):
                _force_writable(Path(target))
                func(target)
            if path.exists():
                shutil.rmtree(path, onerror=onerror)

        _rmtree(bundle_repo)
        _rmtree(bake_dir)

        try:
            (bundle_repo / "src").mkdir(parents=True)
            (bundle_repo / "include").mkdir()
            (bundle_repo / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(bundle_test_lib C)\n"
                "add_library(bundle_test_lib STATIC src/bundle_test_lib.c)\n"
                "target_include_directories(bundle_test_lib PUBLIC\n"
                "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
                "    $<INSTALL_INTERFACE:include>)\n"
                "include(GNUInstallDirs)\n"
                "install(TARGETS bundle_test_lib\n"
                "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
                "    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
                "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n"
            )
            (bundle_repo / "include" / "bundle_test_lib.h").write_text(
                "#ifndef BUNDLE_TEST_LIB_H\n"
                "#define BUNDLE_TEST_LIB_H\n"
                "int bundle_test_lib_answer(void);\n"
                "#endif\n"
            )
            (bundle_repo / "src" / "bundle_test_lib.c").write_text(
                "#include \"bundle_test_lib.h\"\n"
                "int bundle_test_lib_answer(void) { return 42; }\n"
            )

            git_args = [
                "git",
                "-c", "commit.gpgsign=false",
                "-c", "user.email=bundle@test",
                "-c", "user.name=bundle test",
            ]
            self.run_cmd(["git", "init", "-q"], cwd=bundle_repo)
            self.run_cmd([*git_args, "add", "-A"], cwd=bundle_repo)
            self.run_cmd([*git_args, "commit", "-q", "-m", "init"], cwd=bundle_repo)

            output = self.bake(["--local-env", "run", "."], cwd=project_dir)
            text = self.strip_ansi(output)
            self.assertIn("bundle_answer=42", text)
            self.assertIn("bundle_test_lib", text)

            install_root = (
                bake_dir / "bundles" / "bundle_test_lib" / "default" / "install"
            )
            self.assertTrue(
                install_root.exists() and any(install_root.iterdir()),
                f"Expected bundle install tree under {install_root}",
            )

            second_output = self.bake(["--local-env", "run", "."], cwd=project_dir)
            second_text = self.strip_ansi(second_output)
            self.assertIn("bundle_answer=42", second_text)
            self.assertNotIn("Cloning into", second_text)
            self.assertNotIn("Build files have been written to", second_text)
        finally:
            _rmtree(bundle_repo)
            _rmtree(bake_dir)

    def test_cwd_project_is_not_built_for_other_target(self) -> None:
        # A project.json in the current working directory must be discovered
        # (so it can be resolved as a dependency) but must not be built when
        # the command targets an unrelated project. The cwd project declares a
        # bundle with a bogus repository: preparing it is part of building it,
        # so an eager prepare fails the unrelated build.
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"cwd_not_built_{stamp}"
        cwd_id = f"tmp.cwd_not_built.{stamp}.cwd_app"
        target_id = f"tmp.cwd_not_built.{stamp}.target_app"

        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{cwd_id}\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": {\n"
            "        \"use\": [\"cwd_bundle_lib\"]\n"
            "    },\n"
            "    \"bundle\": {\n"
            "        \"cwd_bundle_lib\": {\n"
            f"            \"repository\": \"{(root / 'missing_repo').as_posix()}\",\n"
            "            \"library\": \"cwd_bundle_lib\"\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text(
            "#include <stdio.h>\n"
            "int main(void) { printf(\"cwd_app_ran\\n\"); return 0; }\n"
        )

        target_dir = root / "sub" / "target_app"
        (target_dir / "src").mkdir(parents=True, exist_ok=True)
        (target_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{target_id}\",\n"
            "    \"type\": \"application\"\n"
            "}\n"
        )
        (target_dir / "src" / "main.c").write_text(
            "#include <stdio.h>\n"
            "int main(void) { printf(\"target_app_ran\\n\"); return 0; }\n"
        )

        try:
            output = self.strip_ansi(
                self.bake(["--local-env", "run", "sub/target_app"], cwd=root)
            )
            self.assertIn("target_app_ran", output)
            self.assertNotIn("cwd_app_ran", output)
            self.assertNotIn(cwd_id, output)
            self.assertFalse(
                (root / ".bake" / "bundles").exists(),
                "Bundles of the cwd project were prepared for an unrelated target",
            )
            self.assertFalse(
                (root / ".bake" / "local_env" / "build" / cwd_id).exists(),
                "The cwd project was built while targeting an unrelated project",
            )
        finally:
            self._rm_tree(root)

    def test_cwd_project_is_built_when_target_depends_on_it(self) -> None:
        # The mirror of test_cwd_project_is_not_built_for_other_target: when
        # the target does depend on the project in the current working
        # directory, that project (including its bundles) must be built.
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"cwd_is_dep_{stamp}"
        cwd_id = f"tmp.cwd_is_dep.{stamp}.cwd_pkg"
        target_id = f"tmp.cwd_is_dep.{stamp}.target_app"
        bundle_repo = root / "bundle_repo"

        (bundle_repo / "src").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "include").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(cwd_bundle_lib C)\n"
            "add_library(cwd_bundle_lib STATIC src/cwd_bundle_lib.c)\n"
            "target_include_directories(cwd_bundle_lib PUBLIC\n"
            "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
            "    $<INSTALL_INTERFACE:include>)\n"
            "include(GNUInstallDirs)\n"
            "install(TARGETS cwd_bundle_lib\n"
            "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
            "    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
            "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n"
        )
        (bundle_repo / "include" / "cwd_bundle_lib.h").write_text(
            "#ifndef CWD_BUNDLE_LIB_H\n"
            "#define CWD_BUNDLE_LIB_H\n"
            "int cwd_bundle_lib_answer(void);\n"
            "#endif\n"
        )
        (bundle_repo / "src" / "cwd_bundle_lib.c").write_text(
            "#include \"cwd_bundle_lib.h\"\n"
            "int cwd_bundle_lib_answer(void) { return 42; }\n"
        )

        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "include").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{cwd_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            "        \"use\": [\"cwd_bundle_lib\"]\n"
            "    },\n"
            "    \"bundle\": {\n"
            "        \"cwd_bundle_lib\": {\n"
            f"            \"repository\": \"{bundle_repo.as_posix()}\",\n"
            "            \"library\": \"cwd_bundle_lib\"\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (root / "include" / "cwd_pkg_api.h").write_text("int cwd_pkg_answer(void);\n")
        (root / "src" / "cwd_pkg.c").write_text(
            "#include <cwd_pkg_api.h>\n"
            "#include <cwd_bundle_lib.h>\n"
            "int cwd_pkg_answer(void) { return cwd_bundle_lib_answer(); }\n"
        )

        target_dir = root / "sub" / "target_app"
        (target_dir / "src").mkdir(parents=True, exist_ok=True)
        (target_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{target_id}\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": {\n"
            f"        \"use\": [\"{cwd_id}\"]\n"
            "    }\n"
            "}\n"
        )
        (target_dir / "src" / "main.c").write_text(
            "#include <stdio.h>\n"
            "#include <cwd_pkg_api.h>\n"
            "int main(void) { printf(\"answer=%d\\n\", cwd_pkg_answer()); return 0; }\n"
        )

        try:
            self._git_init_repo(bundle_repo)
            output = self.strip_ansi(
                self.bake(["--local-env", "run", "sub/target_app"], cwd=root)
            )
            self.assertIn("answer=42", output)
            self.assertIn(cwd_id, output)

            install_root = root / ".bake" / "bundles" / "cwd_bundle_lib" / "default" / "install"
            self.assertTrue(
                install_root.exists() and any(install_root.iterdir()),
                f"Expected bundle install tree under {install_root}",
            )
            self.assertTrue(
                (root / ".bake" / "local_env" / "build" / cwd_id).exists(),
                "The cwd project was not built even though the target depends on it",
            )
        finally:
            self._rm_tree(root)

    @staticmethod
    def _rm_tree(path: Path) -> None:
        def _force_writable(target: Path) -> None:
            try:
                target.chmod(stat.S_IWUSR | stat.S_IRUSR | stat.S_IXUSR)
            except OSError:
                pass

        def onerror(func, target, _exc):
            _force_writable(Path(target))
            func(target)

        if path.exists():
            shutil.rmtree(path, onerror=onerror)

    @classmethod
    def _git_init_repo(cls, path: Path) -> None:
        git_args = [
            "git",
            "-c", "commit.gpgsign=false",
            "-c", "user.email=bundle@test",
            "-c", "user.name=bundle test",
        ]
        cls.run_cmd(["git", "init", "-q"], cwd=path)
        cls.run_cmd([*git_args, "add", "-A"], cwd=path)
        cls.run_cmd([*git_args, "commit", "-q", "-m", "init"], cwd=path)

    def test_cpp_project_uses_per_language_config_for_each_source(self) -> None:
        # A package/app whose language is c++ must still compile .c sources with
        # the lang.c configuration (and .cc sources with lang.cpp), rather than
        # forcing every unit through lang.cpp.
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"mixed_lang_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.mixed.lang.{stamp}\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": { \"language\": \"c++\" },\n"
            "    \"lang.c\": { \"defines\": [\"FROM_LANG_C\"] },\n"
            "    \"lang.cpp\": { \"defines\": [\"FROM_LANG_CPP\"], \"cpp-standard\": \"c++11\" }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text(
            "#ifndef FROM_LANG_C\n"
            "#error \"lang.c define not applied to .c source in a c++ project\"\n"
            "#endif\n"
            "#ifdef FROM_LANG_CPP\n"
            "#error \"lang.cpp define leaked into .c source\"\n"
            "#endif\n"
            "int cpp_helper(void);\n"
            "int main(void) { return cpp_helper(); }\n"
        )
        (root / "src" / "helper.cc").write_text(
            "#ifndef FROM_LANG_CPP\n"
            "#error \"lang.cpp define not applied to .cc source\"\n"
            "#endif\n"
            "extern \"C\" int cpp_helper(void) { return 0; }\n"
        )
        try:
            self.bake(["--local-env", "build", "."], cwd=root)
        finally:
            self._rm_tree(root)

    def test_c_app_links_cpp_package_dependency(self) -> None:
        # A C application that uses a C++ package must be linked with the C++
        # driver so the C++ runtime (libc++/libstdc++) is resolved.
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"c_links_cpp_{stamp}"
        pkg = root / "cpplib"
        app = root / "capp"
        pkg_id = f"tmp.cpplib.{stamp}"
        app_id = f"tmp.capp.{stamp}"

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{pkg_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": { \"language\": \"c++\" }\n"
            "}\n"
        )
        (pkg / "include" / "cpplib.h").write_text(
            "#ifndef CPPLIB_H\n"
            "#define CPPLIB_H\n"
            "#ifdef __cplusplus\n"
            "extern \"C\" {\n"
            "#endif\n"
            "int cpplib_answer(void);\n"
            "#ifdef __cplusplus\n"
            "}\n"
            "#endif\n"
            "#endif\n"
        )
        (pkg / "src" / "lib.cc").write_text(
            "#include \"cpplib.h\"\n"
            "#include <stdexcept>\n"
            "extern \"C\" int cpplib_answer(void) {\n"
            "    try { throw std::runtime_error(\"boom\"); }\n"
            "    catch (const std::exception &) { return 42; }\n"
            "}\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{app_id}\",\n"
            "    \"type\": \"application\",\n"
            f"    \"value\": {{ \"use\": [\"{pkg_id}\"] }}\n"
            "}\n"
        )
        (app / "src" / "main.c").write_text(
            "#include \"cpplib.h\"\n"
            "#include <stdio.h>\n"
            "int main(void) { printf(\"answer=%d\\n\", cpplib_answer()); return 0; }\n"
        )
        try:
            output = self.bake(["--local-env", "run", "capp"], cwd=root)
            self.assertIn("answer=42", self.strip_ansi(output))
        finally:
            self._rm_tree(root)

    def test_header_only_bundle_exposes_root_and_propagates_includes(self) -> None:
        # A header-only bundle that lists extra include subdirs must still expose
        # its source root, and those include paths must propagate to dependents
        # of the package that declares the bundle.
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"hdr_bundle_{stamp}"
        bundle_repo = root / "hdrbundle_repo"
        pkg = root / "midpkg"
        app = root / "topapp"
        pkg_id = f"tmp.midpkg.{stamp}"
        app_id = f"tmp.topapp.{stamp}"

        (bundle_repo / "extra").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "hdrbundle.h").write_text(
            "#ifndef HDRBUNDLE_H\n#define HDRBUNDLE_H\n#define HDRBUNDLE_ROOT 1\n#endif\n"
        )
        (bundle_repo / "extra" / "hdrbundle_extra.h").write_text(
            "#ifndef HDRBUNDLE_EXTRA_H\n#define HDRBUNDLE_EXTRA_H\n#define HDRBUNDLE_EXTRA 2\n#endif\n"
        )
        self._git_init_repo(bundle_repo)

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{pkg_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"bundle\": {\n"
            "        \"hdrbundle\": {\n"
            f"            \"repository\": \"{bundle_repo.as_posix()}\",\n"
            "            \"header-only\": true,\n"
            "            \"include\": [\"extra\"]\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "midpkg.h").write_text(
            "#ifndef MIDPKG_H\n#define MIDPKG_H\nint midpkg_sum(void);\n#endif\n"
        )
        (pkg / "src" / "midpkg.c").write_text(
            "#include \"midpkg.h\"\n"
            "#include <hdrbundle.h>\n"
            "#include <hdrbundle_extra.h>\n"
            "int midpkg_sum(void) { return HDRBUNDLE_ROOT + HDRBUNDLE_EXTRA; }\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{app_id}\",\n"
            "    \"type\": \"application\",\n"
            f"    \"value\": {{ \"use\": [\"{pkg_id}\"] }}\n"
            "}\n"
        )
        # main.c relies on the bundle include paths reaching it via midpkg.
        (app / "src" / "main.c").write_text(
            "#include \"midpkg.h\"\n"
            "#include <hdrbundle.h>\n"
            "#include <hdrbundle_extra.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            "    printf(\"sum=%d direct=%d\\n\", midpkg_sum(), HDRBUNDLE_ROOT + HDRBUNDLE_EXTRA);\n"
            "    return 0;\n"
            "}\n"
        )
        try:
            output = self.bake(["--local-env", "run", "topapp"], cwd=root)
            text = self.strip_ansi(output)
            self.assertIn("sum=3", text)
            self.assertIn("direct=3", text)
        finally:
            self._rm_tree(root)

    def test_cmake_bundle_link_inputs_propagate_to_dependents(self) -> None:
        # The library produced by a cmake bundle must be linkable from dependents
        # of the package that declares it (the bundle's libpath/lib propagate).
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"cmake_bundle_prop_{stamp}"
        bundle_repo = root / "calc_repo"
        pkg = root / "mathpkg"
        app = root / "mathapp"
        pkg_id = f"tmp.mathpkg.{stamp}"
        app_id = f"tmp.mathapp.{stamp}"

        (bundle_repo / "src").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "include").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(calc C)\n"
            "add_library(calc STATIC src/calc.c)\n"
            "target_include_directories(calc PUBLIC\n"
            "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
            "    $<INSTALL_INTERFACE:include>)\n"
            "include(GNUInstallDirs)\n"
            "install(TARGETS calc\n"
            "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
            "    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
            "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n"
        )
        (bundle_repo / "include" / "calc.h").write_text(
            "#ifndef CALC_H\n#define CALC_H\nint calc_add(int a, int b);\n#endif\n"
        )
        (bundle_repo / "src" / "calc.c").write_text(
            "#include \"calc.h\"\nint calc_add(int a, int b) { return a + b; }\n"
        )
        self._git_init_repo(bundle_repo)

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{pkg_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"bundle\": {\n"
            "        \"calc\": {\n"
            f"            \"repository\": \"{bundle_repo.as_posix()}\",\n"
            "            \"library\": \"calc\"\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "mathpkg.h").write_text(
            "#ifndef MATHPKG_H\n#define MATHPKG_H\nint mathpkg_compute(void);\n#endif\n"
        )
        # mathpkg.c references calc_add; the symbol must be resolved at the
        # final link of any dependent application.
        (pkg / "src" / "mathpkg.c").write_text(
            "#include \"mathpkg.h\"\n"
            "#include \"calc.h\"\n"
            "int mathpkg_compute(void) { return calc_add(40, 2); }\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{app_id}\",\n"
            "    \"type\": \"application\",\n"
            f"    \"value\": {{ \"use\": [\"{pkg_id}\"] }}\n"
            "}\n"
        )
        (app / "src" / "main.c").write_text(
            "#include \"mathpkg.h\"\n"
            "#include <stdio.h>\n"
            "int main(void) { printf(\"compute=%d\\n\", mathpkg_compute()); return 0; }\n"
        )
        try:
            output = self.bake(["--local-env", "run", "mathapp"], cwd=root)
            self.assertIn("compute=42", self.strip_ansi(output))
        finally:
            self._rm_tree(root)

    def test_cmake_bundle_rebuilds_when_source_changes(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"cmake_bundle_rebuild_{stamp}"
        bundle_repo = root / "calc_repo"
        pkg = root / "mathpkg"
        app = root / "mathapp"
        pkg_id = f"tmp.mathpkg.{stamp}"
        app_id = f"tmp.mathapp.{stamp}"

        (bundle_repo / "src").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "include").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(calc C)\n"
            "add_library(calc STATIC src/calc.c)\n"
            "target_include_directories(calc PUBLIC\n"
            "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
            "    $<INSTALL_INTERFACE:include>)\n"
            "include(GNUInstallDirs)\n"
            "install(TARGETS calc\n"
            "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
            "    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
            "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n"
        )
        (bundle_repo / "include" / "calc.h").write_text(
            "#ifndef CALC_H\n#define CALC_H\nint calc_value(void);\n#endif\n"
        )
        (bundle_repo / "src" / "calc.c").write_text(
            "#include \"calc.h\"\nint calc_value(void) { return 42; }\n"
        )
        self._git_init_repo(bundle_repo)

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{pkg_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"bundle\": {\n"
            "        \"calc\": {\n"
            f"            \"repository\": \"{bundle_repo.as_posix()}\",\n"
            "            \"library\": \"calc\"\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "mathpkg.h").write_text(
            "#ifndef MATHPKG_H\n#define MATHPKG_H\nint mathpkg_compute(void);\n#endif\n"
        )
        (pkg / "src" / "mathpkg.c").write_text(
            "#include \"mathpkg.h\"\n"
            "#include \"calc.h\"\n"
            "int mathpkg_compute(void) { return calc_value(); }\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{app_id}\",\n"
            "    \"type\": \"application\",\n"
            f"    \"value\": {{ \"use\": [\"{pkg_id}\"] }}\n"
            "}\n"
        )
        (app / "src" / "main.c").write_text(
            "#include \"mathpkg.h\"\n"
            "#include <stdio.h>\n"
            "int main(void) { printf(\"value=%d\\n\", mathpkg_compute()); return 0; }\n"
        )
        try:
            output = self.bake(["--local-env", "run", "mathapp"], cwd=root)
            self.assertIn("value=42", self.strip_ansi(output))

            checkouts = list(pkg.glob(".bake/bundles/**/src/src/calc.c"))
            self.assertEqual(
                len(checkouts), 1,
                f"expected one checked-out bundle calc.c, found {checkouts}")
            checkouts[0].write_text(
                "#include \"calc.h\"\nint calc_value(void) { return 43; }\n")

            self.bake(["--local-env", "rebuild", "mathapp"], cwd=root)
            output = self.bake(["--local-env", "run", "mathapp"], cwd=root)
            self.assertIn(
                "value=43", self.strip_ansi(output),
                "bundle library was not rebuilt after its checked-out source "
                "changed; consumer still links the stale library. The "
                "'already built' marker is keyed on the bundle's project.json "
                "config only, never on its source state.")
        finally:
            self._rm_tree(root)

    def write_cmake_bundle_source(self, base: Path, value: int) -> None:
        """Write a cmake static library whose calc_value() returns `value`."""
        (base / "src").mkdir(parents=True, exist_ok=True)
        (base / "include").mkdir(parents=True, exist_ok=True)
        (base / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(calc C)\n"
            "add_library(calc STATIC src/calc.c)\n"
            "target_include_directories(calc PUBLIC\n"
            "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
            "    $<INSTALL_INTERFACE:include>)\n"
            "include(GNUInstallDirs)\n"
            "install(TARGETS calc\n"
            "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
            "    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
            "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n"
        )
        (base / "include" / "calc.h").write_text(
            "#ifndef CALC_H\n#define CALC_H\nint calc_value(void);\n#endif\n"
        )
        (base / "src" / "calc.c").write_text(
            f'#include "calc.h"\nint calc_value(void) {{ return {value}; }}\n'
        )

    def write_cmake_bundle_consumer(
        self, root: Path, stamp: int, bundle_body: str
    ) -> str:
        """Write a package carrying `bundle_body` plus an app that prints the
        bundle's calc_value(). Returns the app id."""
        pkg = root / "mathpkg"
        app = root / "mathapp"
        pkg_id = f"tmp.mathpkg.{stamp}"
        app_id = f"tmp.mathapp.{stamp}"

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f'    "id": "{pkg_id}",\n'
            '    "type": "package",\n'
            '    "bundle": {\n'
            f'        "calc": {bundle_body}\n'
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "mathpkg.h").write_text(
            "#ifndef MATHPKG_H\n#define MATHPKG_H\nint mathpkg_compute(void);\n#endif\n"
        )
        (pkg / "src" / "mathpkg.c").write_text(
            '#include "mathpkg.h"\n'
            '#include "calc.h"\n'
            "int mathpkg_compute(void) { return calc_value(); }\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f'    "id": "{app_id}",\n'
            '    "type": "application",\n'
            f'    "value": {{ "use": ["{pkg_id}"] }}\n'
            "}\n"
        )
        (app / "src" / "main.c").write_text(
            '#include "mathpkg.h"\n'
            "#include <stdio.h>\n"
            'int main(void) { printf("value=%d\\n", mathpkg_compute()); return 0; }\n'
        )
        return app_id

    def require_cmake_bundle_tools(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

    def write_refresh_bundle_workspace(
        self, name: str
    ) -> tuple[Path, Path, Path, Path]:
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        repo = root / "bundle_repo"
        project = root / "app"
        bundle_id = "refresh_bundle"

        (repo / "include").mkdir(parents=True)
        (repo / "include" / "refresh.h").write_text(
            "#ifndef REFRESH_H\n"
            "#define REFRESH_H\n"
            "#define REFRESH_VALUE 1\n"
            "#endif\n"
        )
        self._git_init_repo(repo)
        self.run_cmd(["git", "checkout", "-q", "-b", "tracked"], cwd=repo)

        (project / "src").mkdir(parents=True)
        (project / "project.json").write_text(
            "{\n"
            f'    "id": "tmp.bundle.refresh.{stamp}",\n'
            '    "type": "application",\n'
            f'    "value": {{ "use": ["{bundle_id}"] }},\n'
            '    "bundle": {\n'
            f'        "{bundle_id}": {{\n'
            '            "repository": "../bundle_repo",\n'
            '            "branch": "tracked",\n'
            '            "header-only": true,\n'
            '            "include": ["include"]\n'
            "        }\n"
            "    }\n"
            "}\n"
        )
        (project / "src" / "main.c").write_text(
            "#include <refresh.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            "    printf(\"value=%d\\n\", REFRESH_VALUE);\n"
            "    return 0;\n"
            "}\n"
        )

        checkout = (
            project / ".bake" / "bundles" / bundle_id /
            "branches" / "tracked" / "src"
        )
        return root, project, repo, checkout

    def advance_refresh_bundle(self, repo: Path, value: int) -> str:
        (repo / "include" / "refresh.h").write_text(
            "#ifndef REFRESH_H\n"
            "#define REFRESH_H\n"
            f"#define REFRESH_VALUE {value}\n"
            "#endif\n"
        )
        self.git_commit_all(repo, f"set value to {value}")
        return self.run_cmd(["git", "rev-parse", "HEAD"], cwd=repo).strip()

    def test_bundle_build_notices_local_remote_is_ahead(self) -> None:
        _, project, repo, _ = self.write_refresh_bundle_workspace(
            "bundle_behind_notice")
        self.bake(["build", "."], cwd=project)
        self.advance_refresh_bundle(repo, 2)

        output = self.strip_ansi(self.bake(["build", "."], cwd=project))
        self.assertIn(
            "bundle 'refresh_bundle' checkout is behind its remote branch "
            "'tracked'; run bake3 bundle update refresh_bundle",
            output,
        )

    def test_bundle_update_fast_forwards_checkout(self) -> None:
        _, project, repo, checkout = self.write_refresh_bundle_workspace(
            "bundle_update_fast_forward")
        self.bake(["build", "."], cwd=project)
        old_head = self.run_cmd(
            ["git", "rev-parse", "HEAD"], cwd=checkout).strip()
        new_head = self.advance_refresh_bundle(repo, 2)

        output = self.strip_ansi(
            self.bake(["bundle", "update", "refresh_bundle"], cwd=project))
        self.assertIn(
            f"[bake] bundle 'refresh_bundle' updated {old_head[:12]} -> "
            f"{new_head[:12]} on branch 'tracked'",
            output,
        )
        checkout_head = self.run_cmd(
            ["git", "rev-parse", "HEAD"], cwd=checkout).strip()
        self.assertEqual(checkout_head, new_head)
        self.assertIn(
            "#define REFRESH_VALUE 2",
            (checkout / "include" / "refresh.h").read_text(),
        )
        run_output = self.strip_ansi(self.bake(["run", "."], cwd=project))
        self.assertIn("value=2", run_output)

    def test_bundle_update_refuses_dirty_checkout(self) -> None:
        _, project, _, checkout = self.write_refresh_bundle_workspace(
            "bundle_update_dirty")
        self.bake(["build", "."], cwd=project)
        checkout_head = self.run_cmd(
            ["git", "rev-parse", "HEAD"], cwd=checkout).strip()
        dirty_header = checkout / "include" / "refresh.h"
        dirty_header.write_text(
            "#ifndef REFRESH_H\n"
            "#define REFRESH_H\n"
            "#define REFRESH_VALUE 9\n"
            "#endif\n"
        )

        output = self.strip_ansi(self.bake_expect_failure(
            ["bundle", "update", "refresh_bundle"], cwd=project))
        self.assertIn(
            "bundle 'refresh_bundle' checkout has local modifications; "
            "refusing to update",
            output,
        )
        current_head = self.run_cmd(
            ["git", "rev-parse", "HEAD"], cwd=checkout).strip()
        self.assertEqual(current_head, checkout_head)
        self.assertIn("#define REFRESH_VALUE 9", dirty_header.read_text())

    def test_bundle_update_refuses_diverged_checkout(self) -> None:
        _, project, repo, checkout = self.write_refresh_bundle_workspace(
            "bundle_update_diverged")
        self.bake(["build", "."], cwd=project)
        checkout_header = checkout / "include" / "refresh.h"
        checkout_header.write_text(
            "#ifndef REFRESH_H\n"
            "#define REFRESH_H\n"
            "#define REFRESH_VALUE 3\n"
            "#endif\n"
        )
        self.git_commit_all(checkout, "local value")
        checkout_head = self.run_cmd(
            ["git", "rev-parse", "HEAD"], cwd=checkout).strip()
        self.advance_refresh_bundle(repo, 2)

        output = self.strip_ansi(self.bake_expect_failure(
            ["bundle", "update", "refresh_bundle"], cwd=project))
        self.assertIn(
            "bundle 'refresh_bundle' checkout has diverged from branch "
            "'tracked' and cannot be fast-forwarded; refusing to update",
            output,
        )
        current_head = self.run_cmd(
            ["git", "rev-parse", "HEAD"], cwd=checkout).strip()
        self.assertEqual(current_head, checkout_head)
        self.assertIn("#define REFRESH_VALUE 3", checkout_header.read_text())

    def test_bundle_subdir_builds_the_nested_cmake_project(self) -> None:
        self.require_cmake_bundle_tools()
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_subdir_{stamp}"
        repo = root / "calc_repo"

        self.write_cmake_bundle_source(repo / "libs" / "calc", 21)
        (repo / "README.md").write_text("root of the repository, not a cmake project\n")
        self._git_init_repo(repo)

        self.write_cmake_bundle_consumer(
            root, stamp,
            f'{{ "repository": "{repo.as_posix()}", '
            '"subdir": "libs/calc", "library": "calc" }')

        output = self.strip_ansi(self.bake(["--local-env", "run", "mathapp"], cwd=root))
        self.assertIn("value=21", output)

    def test_bundle_profile_pins_the_cmake_build_type(self) -> None:
        self.require_cmake_bundle_tools()
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_profile_{stamp}"
        repo = root / "calc_repo"

        self.write_cmake_bundle_source(repo, 5)
        self._git_init_repo(repo)

        self.write_cmake_bundle_consumer(
            root, stamp,
            f'{{ "repository": "{repo.as_posix()}", '
            '"library": "calc", "profile": "release" }')

        output = self.strip_ansi(
            self.bake(["--local-env", "--trace", "run", "mathapp"], cwd=root))

        self.assertIn("value=5", output)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", output)
        self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", output)

    def test_bundle_cmake_args_reach_the_configure_command(self) -> None:
        self.require_cmake_bundle_tools()
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_cmake_args_{stamp}"
        repo = root / "calc_repo"

        self.write_cmake_bundle_source(repo, 6)
        (repo / "CMakeLists.txt").write_text(
            (repo / "CMakeLists.txt").read_text() +
            "if(NOT CALC_OPTION)\n"
            '    message(FATAL_ERROR "CALC_OPTION was not passed to cmake")\n'
            "endif()\n"
        )
        self._git_init_repo(repo)

        self.write_cmake_bundle_consumer(
            root, stamp,
            f'{{ "repository": "{repo.as_posix()}", "library": "calc", '
            '"cmake-args": ["-DCALC_OPTION=ON"] }')

        output = self.strip_ansi(
            self.bake(["--local-env", "--trace", "run", "mathapp"], cwd=root))

        self.assertIn("value=6", output)
        self.assertIn("-DCALC_OPTION=ON", output)

    def test_bundle_tag_selects_the_tagged_revision(self) -> None:
        self.require_cmake_bundle_tools()
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_tag_{stamp}"
        repo = root / "calc_repo"

        self.write_cmake_bundle_source(repo, 1)
        self._git_init_repo(repo)
        self.git_tag(repo, "v1")
        self.write_cmake_bundle_source(repo, 2)
        self.git_commit_all(repo, "bump to 2")

        self.write_cmake_bundle_consumer(
            root, stamp,
            f'{{ "repository": "{repo.as_posix()}", "library": "calc", "tag": "v1" }}')

        output = self.strip_ansi(self.bake(["--local-env", "run", "mathapp"], cwd=root))
        self.assertIn("value=1", output)

    def test_bundle_commit_selects_the_pinned_revision(self) -> None:
        self.require_cmake_bundle_tools()
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_commit_{stamp}"
        repo = root / "calc_repo"

        self.write_cmake_bundle_source(repo, 11)
        self._git_init_repo(repo)
        pinned = self.run_cmd(["git", "rev-parse", "HEAD"], cwd=repo).strip()
        self.write_cmake_bundle_source(repo, 12)
        self.git_commit_all(repo, "bump to 12")

        self.write_cmake_bundle_consumer(
            root, stamp,
            f'{{ "repository": "{repo.as_posix()}", "library": "calc", '
            f'"commit": "{pinned}" }}')

        output = self.strip_ansi(self.bake(["--local-env", "run", "mathapp"], cwd=root))
        self.assertIn("value=11", output)

    def test_bundle_branch_selects_the_named_branch(self) -> None:
        self.require_cmake_bundle_tools()
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_branch_{stamp}"
        repo = root / "calc_repo"

        self.write_cmake_bundle_source(repo, 3)
        self._git_init_repo(repo)
        self.run_cmd(["git", "checkout", "-q", "-b", "side"], cwd=repo)
        self.write_cmake_bundle_source(repo, 4)
        self.git_commit_all(repo, "side branch value")

        self.write_cmake_bundle_consumer(
            root, stamp,
            f'{{ "repository": "{repo.as_posix()}", "library": "calc", '
            '"branch": "side" }')

        output = self.strip_ansi(self.bake(["--local-env", "run", "mathapp"], cwd=root))
        self.assertIn("value=4", output)

    def test_bundle_build_system_cargo_builds_a_rust_dependency(self) -> None:
        self.require_cmake_bundle_tools()
        if shutil.which("cargo") is None:
            self.skipTest("cargo not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_cargo_{stamp}"
        repo = root / "calc_repo"
        (repo / "src").mkdir(parents=True)
        (repo / "Cargo.toml").write_text(
            "[package]\n"
            'name = "calc"\n'
            'version = "0.1.0"\n'
            'edition = "2021"\n'
            "\n"
            "[lib]\n"
            'name = "calc"\n'
            'crate-type = ["staticlib"]\n'
        )
        (repo / "src" / "lib.rs").write_text(
            "#[no_mangle]\n"
            "pub extern \"C\" fn calc_value() -> i32 { 33 }\n"
        )
        self._git_init_repo(repo)

        pkg = root / "mathpkg"
        app = root / "mathapp"
        pkg_id = f"tmp.cargopkg.{stamp}"
        app_id = f"tmp.cargoapp.{stamp}"
        (pkg / "src").mkdir(parents=True)
        (pkg / "include").mkdir(parents=True)
        (pkg / "project.json").write_text(
            "{\n"
            f'    "id": "{pkg_id}",\n'
            '    "type": "package",\n'
            '    "bundle": {\n'
            f'        "calc": {{ "repository": "{repo.as_posix()}", '
            '"build-system": "cargo", "library": "calc" }\n'
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "cargopkg.h").write_text(
            "#ifndef CARGOPKG_H\n#define CARGOPKG_H\nint pkg_value(void);\n#endif\n"
        )
        (pkg / "src" / "pkg.c").write_text(
            '#include "cargopkg.h"\n'
            "int calc_value(void);\n"
            "int pkg_value(void) { return calc_value(); }\n"
        )
        (app / "src").mkdir(parents=True)
        (app / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"]}}}}\n'
        )
        (app / "src" / "main.c").write_text(
            '#include "cargopkg.h"\n'
            "#include <stdio.h>\n"
            'int main(void) { printf("value=%d\\n", pkg_value()); return 0; }\n'
        )

        output = self.strip_ansi(self.bake(["--local-env", "run", "mathapp"], cwd=root))
        self.assertIn("value=33", output)

    def test_bundle_sources_are_compiled_into_the_consumer(self) -> None:
        self.require_cmake_bundle_tools()
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"bundle_sources_{stamp}"
        repo = root / "calc_repo"

        (repo / "extra").mkdir(parents=True)
        (repo / "extra" / "extra.c").write_text("int extra_value(void) { return 9; }\n")
        (repo / "extra" / "extra.h").write_text(
            "#ifndef EXTRA_H\n#define EXTRA_H\nint extra_value(void);\n#endif\n"
        )
        self._git_init_repo(repo)

        pkg = root / "mathpkg"
        app = root / "mathapp"
        pkg_id = f"tmp.srcpkg.{stamp}"
        app_id = f"tmp.srcapp.{stamp}"
        (pkg / "src").mkdir(parents=True)
        (pkg / "include").mkdir(parents=True)
        (pkg / "project.json").write_text(
            "{\n"
            f'    "id": "{pkg_id}",\n'
            '    "type": "package",\n'
            '    "bundle": {\n'
            f'        "extra": {{ "repository": "{repo.as_posix()}", '
            '"header-only": true, "include": ["extra"], '
            '"sources": ["extra/extra.c"] }\n'
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "srcpkg.h").write_text(
            "#ifndef SRCPKG_H\n#define SRCPKG_H\nint pkg_value(void);\n#endif\n"
        )
        (pkg / "src" / "pkg.c").write_text(
            '#include "srcpkg.h"\n'
            '#include "extra.h"\n'
            "int pkg_value(void) { return extra_value(); }\n"
        )
        (app / "src").mkdir(parents=True)
        (app / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"]}}}}\n'
        )
        (app / "src" / "main.c").write_text(
            '#include "srcpkg.h"\n'
            "#include <stdio.h>\n"
            'int main(void) { printf("value=%d\\n", pkg_value()); return 0; }\n'
        )

        output = self.strip_ansi(self.bake(["--local-env", "run", "mathapp"], cwd=root))
        self.assertIn("value=9", output)

    def test_discovery_skips_build_directories(self) -> None:
        # project.json files inside a build/ directory (e.g. nested CMake
        # build/_deps trees) must not be discovered as bake projects.
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"disc_skip_{stamp}"
        shared_id = f"tmp.disc.main.{stamp}"

        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{shared_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (root / "src" / "x.c").write_text("int x_value(void) { return 0; }\n")

        # Stray project nested under build/ that shares the main project's id.
        # If discovery descended into build/, this would raise a duplicate-id
        # error and fail the build.
        stray = root / "build" / "_deps" / "nested-src"
        (stray / "src").mkdir(parents=True, exist_ok=True)
        (stray / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{shared_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (stray / "src" / "y.c").write_text("int y_value(void) { return 0; }\n")
        try:
            self.bake(["--local-env", "build", "."], cwd=root)
        finally:
            self._rm_tree(root)

    @unittest.skipIf(platform.system() == "Windows", "POSIX symlinks not supported on this Windows test setup")
    def test_bake_home_with_symlinked_include_does_not_delete_source(self) -> None:
        """When bake_home contains a symlink that points back into the project's
        source include tree (e.g. legacy bake2 ~/bake/include/<id> symlinks),
        bake's sync step must unlink the symlink rather than recurse through it
        and delete the source headers it points to."""
        stamp = int(time.time() * 1_000_000)
        project_id = "myproj_sym"
        tmp_root = self.repo_root / "test" / "tmp" / f"sync_symlink_{stamp}"
        project_dir = tmp_root / "project"
        bake_home = tmp_root / "fake_bake_home"
        include_dir = project_dir / "include"
        nested_include_dir = include_dir / project_id
        src_dir = project_dir / "src"

        nested_include_dir.mkdir(parents=True, exist_ok=True)
        src_dir.mkdir(parents=True, exist_ok=True)
        (bake_home / "include").mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (include_dir / f"{project_id}.h").write_text("extern int x;\n")
        (nested_include_dir / "important.h").write_text("/* important */\n")
        (nested_include_dir / "other.h").write_text("/* other */\n")
        (src_dir / f"{project_id}.c").write_text(f"#include <{project_id}.h>\nint x = 1;\n")

        symlink_path = bake_home / "include" / project_id
        os.symlink(str(nested_include_dir), str(symlink_path))

        before = sorted(p.relative_to(project_dir).as_posix() for p in nested_include_dir.rglob("*") if p.is_file())
        self.assertIn(f"include/{project_id}/important.h", before)

        env = self.env.copy()
        env["BAKE_HOME"] = str(bake_home)
        try:
            self.bake([], cwd=project_dir, env=env)
            after_source = sorted(p.relative_to(project_dir).as_posix() for p in nested_include_dir.rglob("*") if p.is_file())
        finally:
            shutil.rmtree(tmp_root, ignore_errors=True)

        missing = [path for path in before if path not in after_source]
        self.assertEqual(
            missing,
            [],
            "Source include files were deleted via a symlink in bake_home",
        )

    def test_bake_home_inside_project_does_not_delete_source_includes(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = "myproj"
        project_dir = self.repo_root / "test" / "tmp" / f"sync_overlap_{stamp}"
        include_dir = project_dir / "include"
        nested_include_dir = include_dir / project_id
        src_dir = project_dir / "src"

        nested_include_dir.mkdir(parents=True, exist_ok=True)
        src_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (include_dir / f"{project_id}.h").write_text("extern int x;\n")
        (nested_include_dir / "important.h").write_text("/* important header */\n")
        (nested_include_dir / "other.h").write_text("/* another header */\n")
        (src_dir / f"{project_id}.c").write_text(f"#include <{project_id}.h>\nint x = 1;\n")

        before = sorted(p.relative_to(project_dir).as_posix() for p in include_dir.rglob("*") if p.is_file())

        env = self.env.copy()
        env["BAKE_HOME"] = str(project_dir)
        try:
            output = self.bake_expect_failure([], cwd=project_dir, env=env)
            after = sorted(p.relative_to(project_dir).as_posix() for p in include_dir.rglob("*") if p.is_file())
        finally:
            shutil.rmtree(project_dir, ignore_errors=True)

        self.assertIn("refusing to sync tree", self.strip_ansi(output))
        missing = [path for path in before if path not in after]
        self.assertEqual(
            missing,
            [],
            "Source include files were deleted by bake when bake_home overlaps the project path",
        )

    def test_worktree_unchanged_after_run(self) -> None:
        current_snapshot = self.git_snapshot()
        self.assertEqual(
            self.git_snapshot_before,
            current_snapshot,
            "Tracked files changed during run_tests.py execution",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
