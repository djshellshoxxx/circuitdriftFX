#!/usr/bin/env python3
"""Exercise the production CMake gate with owned, local, compiler-free repositories.

No upstream dependency code or network service is used. CMake markers establish
that every negative case is rejected before either dependency is configured.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--git", default="git")
    return parser.parse_known_args()


ARGS, UNITTEST_ARGS = arguments()
HELPER = Path(__file__).resolve().parents[2] / "cmake" / "PinnedDependencies.cmake"


def run(command, *, cwd=None, env=None):
    return subprocess.run(command, cwd=cwd, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout.strip()


def git(path, *args):
    return run([ARGS.git, "-c", "protocol.file.allow=always", "-C", str(path), *args])


def cmake_literal(value):
    return "[==[" + str(value).replace("\\", "/") + "]==]"


class DependencyGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shared = tempfile.TemporaryDirectory(prefix="cdfx-dependency-fixtures-")
        cls.origin = Path(cls.shared.name)
        cls.repositories = {}
        cls.pins = {}
        for name in ("clap", "clap-helpers", "juce", "clap_juce_extensions"):
            path = cls.origin / name
            path.mkdir()
            git(path, "init", "--quiet")
            git(path, "config", "user.name", "Dependency gate fixture")
            git(path, "config", "user.email", "fixture@example.invalid")
            (path / "README.txt").write_text("Owned synthetic dependency fixture.\n")
            (path / ".gitignore").write_text("ignored-input.cmake\n")
            if name == "juce":
                cmake = ('file(APPEND "${CD_CONFIGURED_MARKER}" "juce\\n")\n'
                         'add_library(owned_juce INTERFACE)\n')
            elif name == "clap_juce_extensions":
                cmake = ('if(NOT TARGET owned_juce)\n'
                         '  message(FATAL_ERROR "JUCE must configure first")\n'
                         'endif()\n'
                         'file(APPEND "${CD_CONFIGURED_MARKER}" "clap_juce_extensions\\n")\n')
                for child in ("clap", "clap-helpers"):
                    git(path, "submodule", "add", "--quiet", str(cls.repositories[child]), "clap-libs/" + child)
            else:
                cmake = 'message(FATAL_ERROR "Header submodules must not be configured")\n'
            (path / "CMakeLists.txt").write_text(cmake)
            git(path, "add", ".")
            git(path, "commit", "--quiet", "-m", "Owned pinned dependency fixture")
            cls.repositories[name] = path
            cls.pins[name] = git(path, "rev-parse", "HEAD")

    @classmethod
    def tearDownClass(cls):
        cls.shared.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cdfx-dependency-gate-")
        self.base = Path(self.temp.name) / "path with spaces"
        self.base.mkdir()
        self.cache = self.base / "source-cache"
        self.project = self.base / "project"
        self.project.mkdir()
        self.build = self.base / "build"
        self.marker = self.base / "configured.txt"
        self.sources = {}
        self.overrides = True
        self.expected = dict(self.pins)
        self.fully_disconnected = False
        self.new_policies = False

    def tearDown(self):
        self.temp.cleanup()

    def checkouts(self, *, cache=False):
        parent = self.cache if cache else self.base / "source overrides"
        parent.mkdir()
        for name in ("juce", "clap_juce_extensions"):
            path = parent / (name + "-src")
            run([ARGS.git, "-c", "protocol.file.allow=always", "clone", "--quiet",
                 "--recurse-submodules", str(self.repositories[name]), str(path)])
            self.sources[name] = path
        self.overrides = not cache
        return self.sources

    def configure(self, *, success):
        values = {
            "CD_DEPS_CACHE_DIR": self.cache,
            "CD_CONFIGURED_MARKER": self.marker,
            "GIT_EXECUTABLE": ARGS.git,
            "CD_JUCE_REPO": self.repositories["juce"],
            "CD_CJE_REPO": self.repositories["clap_juce_extensions"],
            "CD_JUCE_TAG_NAME": "owned-test",
            "CD_JUCE_SHA": self.expected["juce"],
            "CD_CJE_SHA": self.expected["clap_juce_extensions"],
            "CD_CLAP_SHA": self.expected["clap"],
            "CD_CLAP_HELPERS_SHA": self.expected["clap-helpers"],
        }
        if self.overrides:
            values.update({"FETCHCONTENT_SOURCE_DIR_" + name.upper(): path
                           for name, path in self.sources.items()})
        if self.fully_disconnected:
            values["FETCHCONTENT_FULLY_DISCONNECTED"] = "ON"
        source = "cmake_minimum_required(VERSION 3.22)\nproject(DependencyGateFixture LANGUAGES NONE)\n"
        if self.new_policies:
            source += ("foreach(policy CMP0168 CMP0169 CMP0170)\n"
                       "  if(POLICY ${policy})\n"
                       "    cmake_policy(SET ${policy} NEW)\n"
                       "  endif()\nendforeach()\n")
        source += "".join("set(" + name + " " + cmake_literal(value) + ")\n"
                          for name, value in values.items())
        source += "include(" + cmake_literal(HELPER) + ")\ncd_add_pinned_plugin_dependencies()\n"
        (self.project / "CMakeLists.txt").write_text(source)
        # Only these owned local repositories may use Git's file transport.
        env = dict(os.environ)
        env.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="protocol.file.allow", GIT_CONFIG_VALUE_0="always")
        result = subprocess.run([ARGS.cmake, "-S", str(self.project), "-B", str(self.build)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(self.marker.read_text(), "juce\nclap_juce_extensions\n")
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertFalse(self.marker.exists(), "Dependency configuration ran before rejection:\n" + result.stdout)
        return result.stdout

    def test_clean_source_overrides_are_accepted_read_only(self):
        self.checkouts()
        self.configure(success=True)
        for path in self.sources.values():
            self.assertEqual(git(path, "status", "--porcelain", "--ignored", "--untracked-files=all"), "")
        self.assertFalse(self.cache.exists())

    def test_clean_warm_source_cache_needs_no_population_subbuild(self):
        self.checkouts(cache=True)
        self.fully_disconnected = True
        self.configure(success=True)
        self.assertFalse((self.build / "_deps" / "juce-subbuild").exists())
        self.assertEqual({p.name for p in self.cache.iterdir()}, {"juce-src", "clap_juce_extensions-src"})

    def test_fresh_local_population_keeps_executable_state_out_of_source_cache(self):
        self.overrides = False
        self.configure(success=True)
        self.assertEqual({p.name for p in self.cache.iterdir()}, {"juce-src", "clap_juce_extensions-src"})
        for name in ("juce", "clap_juce_extensions"):
            self.assertTrue((self.build / "_deps" / (name + "-subbuild")).is_dir())

    def test_fresh_population_with_new_cmake_policies(self):
        self.overrides = False
        self.new_policies = True
        self.configure(success=True)
        self.assertEqual({p.name for p in self.cache.iterdir()}, {"juce-src", "clap_juce_extensions-src"})

    def test_missing_disconnected_source_fails_without_fetch(self):
        self.overrides = False
        self.fully_disconnected = True
        output = self.configure(success=False)
        self.assertIn("while fully disconnected", output)
        self.assertFalse(self.cache.exists())

    def test_missing_override_is_not_replaced_with_download(self):
        self.checkouts()
        self.sources["juce"] = self.base / "missing checkout"
        self.assertIn("source override is not a directory", self.configure(success=False))
        self.assertFalse(self.cache.exists())

    def test_incorrect_root_pin_is_rejected(self):
        self.checkouts()
        self.expected["juce"] = "0" * 40
        self.assertIn("expected " + "0" * 40, self.configure(success=False))

    def test_wrong_cached_revision_is_preserved_and_rejected(self):
        self.checkouts(cache=True)
        path = self.sources["juce"]
        (path / "README.txt").write_text("Another owned fixture revision.\n")
        git(path, "add", "README.txt")
        git(path, "-c", "user.name=Dependency gate fixture", "-c", "user.email=fixture@example.invalid",
            "commit", "--quiet", "-m", "A different owned revision")
        before = git(path, "rev-parse", "HEAD")
        self.assertNotEqual(before, self.pins["juce"])
        self.assertIn("JUCE owned-test: expected", self.configure(success=False))
        self.assertEqual(git(path, "rev-parse", "HEAD"), before)

    def test_incorrect_second_root_pin_precedes_either_configuration(self):
        self.checkouts()
        self.expected["clap_juce_extensions"] = "0" * 40
        self.assertIn("expected " + "0" * 40, self.configure(success=False))

    def test_incorrect_clap_pin_precedes_either_configuration(self):
        self.checkouts()
        self.expected["clap"] = "0" * 40
        self.assertIn("CLAP headers: expected", self.configure(success=False))

    def test_incorrect_helpers_pin_precedes_either_configuration(self):
        self.checkouts()
        self.expected["clap-helpers"] = "0" * 40
        self.assertIn("clap-helpers: expected", self.configure(success=False))

    def test_tracked_root_change_is_rejected_without_repair(self):
        self.checkouts(cache=True)
        changed = self.sources["juce"] / "CMakeLists.txt"
        changed.write_text(changed.read_text() + "# local edit\n")
        before = changed.read_bytes()
        self.assertIn("source checkout is not clean", self.configure(success=False))
        self.assertEqual(changed.read_bytes(), before)

    def test_staged_change_is_rejected(self):
        self.checkouts()
        path = self.sources["juce"]
        (path / "README.txt").write_text("staged local edit\n")
        git(path, "add", "README.txt")
        self.assertIn("source checkout is not clean", self.configure(success=False))

    def test_untracked_root_input_is_rejected(self):
        self.checkouts()
        (self.sources["clap_juce_extensions"] / "new-input.cmake").write_text("# local input\n")
        self.assertIn("source checkout is not clean", self.configure(success=False))

    def test_ignored_input_is_rejected(self):
        self.checkouts()
        (self.sources["juce"] / "ignored-input.cmake").write_text("# ignored local input\n")
        self.assertIn("source checkout is not clean", self.configure(success=False))

    def test_submodule_tracked_change_is_rejected(self):
        self.checkouts()
        path = self.sources["clap_juce_extensions"] / "clap-libs" / "clap"
        (path / "README.txt").write_text("local submodule edit\n")
        self.assertIn("source checkout is not clean", self.configure(success=False))

    def test_submodule_ignored_input_is_rejected(self):
        self.checkouts()
        path = self.sources["clap_juce_extensions"] / "clap-libs" / "clap-helpers"
        (path / "ignored-input.cmake").write_text("# ignored local submodule input\n")
        self.assertIn("clap-helpers: source checkout is not clean", self.configure(success=False))

    def test_missing_submodule_checkout_is_rejected(self):
        self.checkouts()
        path = self.sources["clap_juce_extensions"]
        git(path, "submodule", "deinit", "--force", "clap-libs/clap")
        self.assertIn("CLAP headers: expected a Git checkout", self.configure(success=False))

    def test_missing_helpers_checkout_is_rejected(self):
        self.checkouts()
        path = self.sources["clap_juce_extensions"]
        git(path, "submodule", "deinit", "--force", "clap-libs/clap-helpers")
        self.assertIn("clap-helpers: expected a Git checkout", self.configure(success=False))

    def test_missing_root_git_metadata_is_rejected(self):
        self.checkouts()
        path = self.sources["juce"]
        (path / ".git").rename(path / "git-metadata-saved")
        self.assertIn("expected a Git checkout", self.configure(success=False))

    def test_git_worktree_must_identify_the_configured_source(self):
        self.checkouts()
        path = self.sources["juce"]
        git(path, "config", "core.worktree", str(self.repositories["juce"]))
        self.assertIn("Git worktree does not match source directory", self.configure(success=False))

    def test_assume_unchanged_index_entry_is_rejected(self):
        self.checkouts()
        path = self.sources["juce"]
        git(path, "update-index", "--assume-unchanged", "CMakeLists.txt")
        (path / "CMakeLists.txt").write_text("# local edit hidden by index flag\n")
        self.assertIn("cannot verify index entries", self.configure(success=False))

    def test_skip_worktree_index_entry_is_rejected(self):
        self.checkouts()
        path = self.sources["juce"]
        git(path, "update-index", "--skip-worktree", "CMakeLists.txt")
        (path / "CMakeLists.txt").write_text("# local edit hidden by index flag\n")
        self.assertIn("cannot verify index entries", self.configure(success=False))


if __name__ == "__main__":
    unittest.main(argv=[__file__, *UNITTEST_ARGS], verbosity=2)
