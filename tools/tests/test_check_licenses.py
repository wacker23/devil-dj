"""Tests for tools/check_licenses.py. Run: python -m unittest discover -s tools/tests"""

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import check_licenses as cl  # noqa: E402


class FakeRepo:
    """Copy of the real manifests + LICENSES.md in a temp dir, so tests can break them."""

    def __init__(self):
        self.root = Path(tempfile.mkdtemp(prefix="ddj_lic_"))
        (self.root / "cmake").mkdir()
        (self.root / "docs").mkdir()
        shutil.copy(REPO / "cmake" / "Dependencies.cmake", self.root / "cmake")
        shutil.copy(REPO / "vcpkg.json", self.root)
        shutil.copy(REPO / "docs" / "LICENSES.md", self.root / "docs")

    def add_vcpkg_dependency(self, name):
        path = self.root / "vcpkg.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        data["dependencies"].append(name)
        path.write_text(json.dumps(data, indent=2), encoding="utf-8")

    def add_allowlist_row(self, name, licence):
        path = self.root / "docs" / "LICENSES.md"
        text = path.read_text(encoding="utf-8")
        marker = "| catch2 |"
        text = text.replace(marker, f"| {name} | vcpkg | {licence} | yes |\n{marker}", 1)
        path.write_text(text, encoding="utf-8")

    def add_fetchcontent(self, name):
        path = self.root / "cmake" / "Dependencies.cmake"
        path.write_text(path.read_text(encoding="utf-8")
                        + f"\nFetchContent_Declare({name} GIT_REPOSITORY https://example.invalid/{name}.git)\n",
                        encoding="utf-8")

    def run(self, *extra):
        return cl.main(["--repo", str(self.root), *extra])

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


class CheckLicensesTests(unittest.TestCase):
    def setUp(self):
        self.repo = FakeRepo()

    def tearDown(self):
        self.repo.cleanup()

    def test_real_repository_passes(self):
        self.assertEqual(cl.main(["--repo", str(REPO)]), 0)

    def test_unlisted_vcpkg_dependency_fails(self):
        self.repo.add_vcpkg_dependency("fake-gpl-lib")
        self.assertEqual(self.repo.run(), 1)

    def test_fake_gpl_entry_fails(self):
        # The P0-07 "Done when" case: a dependency listed with a GPL licence must fail the check.
        self.repo.add_vcpkg_dependency("fake-gpl-lib")
        self.repo.add_allowlist_row("fake-gpl-lib", "GPL-3.0-only")
        self.assertEqual(self.repo.run(), 1)

    def test_forbidden_licence_families_fail(self):
        for licence in ["AGPL-3.0-or-later", "LGPL-2.1-only", "GPL-2.0", "CC-BY-NC-SA-4.0", "SSPL-1.0"]:
            with self.subTest(licence=licence):
                errors = cl.check([cl.Dependency("x", "test")], {"x": licence})
                self.assertEqual(len(errors), 1)
                self.assertIn("forbidden", errors[0])

    def test_unknown_licence_fails(self):
        errors = cl.check([cl.Dependency("x", "test")], {"x": "WTFPL"})
        self.assertEqual(len(errors), 1)
        self.assertIn("not in the allowed set", errors[0])

    def test_allowed_licences_pass(self):
        for licence in ["MIT", "BSD-3-Clause", "Apache-2.0", "MPL-1.1", "Zlib", "Public-Domain"]:
            with self.subTest(licence=licence):
                self.assertEqual(cl.check([cl.Dependency("x", "test")], {"x": licence}), [])

    def test_unlisted_fetchcontent_dependency_fails(self):
        self.repo.add_fetchcontent("mystery")
        self.assertEqual(self.repo.run(), 1)

    def test_commented_out_fetchcontent_is_ignored(self):
        self.assertEqual(cl.parse_fetchcontent("# FetchContent_Declare(ghost ...)\nFetchContent_Declare(juce)"),
                         ["juce"])

    def test_vcpkg_object_dependencies_are_read(self):
        manifest = json.dumps({"dependencies": ["a", {"name": "B", "features": ["x"]}]})
        self.assertEqual(cl.parse_vcpkg_manifest(manifest), ["a", "b"])

    def test_unlisted_transitive_port_fails(self):
        installed = self.repo.root / "installed" / "x64-windows-static-md"
        for port in ["sqlite3", "sneaky-transitive"]:
            (installed / "share" / port).mkdir(parents=True)
            (installed / "share" / port / "vcpkg.spdx.json").write_text("{}", encoding="utf-8")
        self.assertEqual(self.repo.run("--vcpkg-installed", str(installed)), 1)

    def test_missing_table_is_an_error(self):
        (self.repo.root / "docs" / "LICENSES.md").write_text("# nothing here\n", encoding="utf-8")
        self.assertEqual(self.repo.run(), 2)


if __name__ == "__main__":
    unittest.main()
