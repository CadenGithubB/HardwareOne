#!/usr/bin/env python3
"""Sequential cross-target builds in one checkout resync managed_components.

Every build directory shares <repo>/managed_components, which the component
manager syncs at configure time to that build's lock, and the per-target locks
pin different versions. On 2026-11-08 a P4 configure replaced libsodium 1.0.22
with 1.0.20 and the next incremental FeatherS3 build failed in ninja on a
missing keccak1600.c, because nothing told the S3 build directory to
reconfigure.

Runs the real cmake/HW1ManagedComponents.cmake helper with cmake -P (no
ESP-IDF): the stamp changes exactly when a different lock populates the
directory (so other build directories reconfigure) and is left untouched when
the same lock configures again (so incremental builds stay incremental). Also
checks that two configures never sync at once, the root CMakeLists.txt wiring,
and the checkout build lock that tools/build_board.sh and
tools/build_deployment.sh take: builds of one target may share it, a build of
another target is refused while one runs.
"""

from pathlib import Path
import os
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[4]
HELPER = REPO / "cmake" / "HW1ManagedComponents.cmake"
LOCK_HELPER = REPO / "tools" / "checkout_build_lock.sh"
# CTest passes its own cmake (HW1_CMAKE), which need not be on PATH; a set but
# missing HW1_CMAKE fails the run instead of skipping the stamp tests.
CMAKE = os.environ.get("HW1_CMAKE") or shutil.which("cmake")
if os.environ.get("HW1_CMAKE") and not os.access(CMAKE, os.X_OK):
    raise SystemExit(f"HW1_CMAKE={CMAKE} is not an executable cmake")
OLD = 1_000_000_000  # an mtime long before any write in this test


@unittest.skipUnless(CMAKE, "cmake is required")
class ManagedComponentsStamp(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stamp = self.root / ".hw1-build" / "managed_components.stamp"
        self.s3_lock = self.root / "dependencies.lock"
        self.s3_lock.write_text("target: esp32s3\nlibsodium: 1.0.22~1\n")
        self.p4_lock = self.root / "dependencies.lock.esp32p4"
        self.p4_lock.write_text("target: esp32p4\nlibsodium: 1.0.20~4\n")

    def configure(self, lock, phase="PRE_SYNC", then=""):
        script = self.root / "stamp.cmake"
        script.write_text(f'''cmake_minimum_required(VERSION 3.16)
include("{HELPER}")
hw1_managed_components_stamp("{lock}" "{self.stamp}" {phase})
{then}
''')
        result = subprocess.run([CMAKE, "-P", str(script)],
                                capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def sha(self, path):
        import hashlib
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def age_stamp(self):
        os.utime(self.stamp, (OLD, OLD))

    def test_first_configure_records_its_lock(self):
        code, out = self.configure(self.s3_lock)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.stamp.read_text(), self.sha(self.s3_lock) + "\n")

    def test_same_lock_leaves_stamp_untouched(self):
        # All S3 boards (or two P4 build directories) share one solved lock:
        # their configures must not make each other reconfigure.
        for phase in ("PRE_SYNC", "POST_SYNC"):
            self.assertEqual(self.configure(self.s3_lock, phase)[0], 0)
        self.age_stamp()
        for phase in ("PRE_SYNC", "POST_SYNC"):
            code, out = self.configure(self.s3_lock, phase)
            self.assertEqual(code, 0, out)
        self.assertEqual(self.stamp.stat().st_mtime, OLD)

    def test_other_lock_rewrites_stamp_and_back(self):
        self.configure(self.s3_lock)
        self.age_stamp()
        self.configure(self.p4_lock)  # a P4 build resyncs the directory
        self.assertEqual(self.stamp.read_text(), self.sha(self.p4_lock) + "\n")
        self.assertGreater(self.stamp.stat().st_mtime, OLD)
        self.age_stamp()
        self.configure(self.s3_lock)  # the S3 build then resyncs it back
        self.assertEqual(self.stamp.read_text(), self.sha(self.s3_lock) + "\n")
        self.assertGreater(self.stamp.stat().st_mtime, OLD)

    def test_resolved_lock_is_recorded_after_sync(self):
        # A first P4 solve rewrites the seeded lock (it records the local
        # ESP-DL override); the post-sync call records the result.
        self.configure(self.p4_lock, "PRE_SYNC")
        self.p4_lock.write_text(self.p4_lock.read_text() + "esp-dl: override\n")
        self.configure(self.p4_lock, "POST_SYNC")
        self.assertEqual(self.stamp.read_text(), self.sha(self.p4_lock) + "\n")

    def test_missing_lock_invalidates(self):
        self.configure(self.s3_lock)
        code, out = self.configure(self.root / "not-solved-yet.lock")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.stamp.read_text(), "unsolved\n")

    def test_unknown_phase_is_an_error(self):
        code, _ = self.configure(self.s3_lock, "LATER")
        self.assertNotEqual(code, 0)

    def test_sync_is_serialized_until_post_sync(self):
        # A configure between PRE_SYNC and POST_SYNC holds <stamp>.lock; a
        # second one cannot take it, and can once POST_SYNC released it.
        probe = (f'file(LOCK "{self.stamp}.lock" GUARD PROCESS TIMEOUT 0 '
                 'RESULT_VARIABLE r)\nmessage("probe=${r}")')
        probe_script = self.root / "probe.cmake"
        probe_script.write_text(probe)
        hold = (f'execute_process(COMMAND "{CMAKE}" -P "{probe_script}")\n'
                f'hw1_managed_components_stamp("{self.s3_lock}" "{self.stamp}" POST_SYNC)\n'
                f'execute_process(COMMAND "{CMAKE}" -P "{probe_script}")')
        code, out = self.configure(self.s3_lock, "PRE_SYNC", then=hold)
        self.assertEqual(code, 0, out)
        probes = [line for line in out.splitlines() if line.startswith("probe=")]
        self.assertEqual(len(probes), 2, out)
        self.assertNotEqual(probes[0], "probe=0", out)  # held during the sync
        self.assertEqual(probes[1], "probe=0", out)     # free after POST_SYNC


class RootWiring(unittest.TestCase):
    def test_root_cmakelists_brackets_the_sync(self):
        text = (REPO / "CMakeLists.txt").read_text()
        lock_set = text.find('idf_build_set_property(DEPENDENCIES_LOCK')
        pre = text.find("PRE_SYNC)")
        project = text.find("project(hardwareone-idf)")
        post = text.find("POST_SYNC)")
        for name, pos in (("DEPENDENCIES_LOCK", lock_set), ("PRE_SYNC", pre),
                          ("project()", project), ("POST_SYNC", post)):
            self.assertNotEqual(pos, -1, f"{name} not found in CMakeLists.txt")
        self.assertLess(lock_set, pre)
        self.assertLess(pre, project)
        self.assertLess(project, post)

    def test_tracked_p4_lock_is_a_configure_input(self):
        # The P4 build solves against a copy in its build directory; a change
        # to the tracked lock alone must still reconfigure (re-seed, resync).
        text = (REPO / "CMakeLists.txt").read_text()
        self.assertIn('set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS '
                      '"${_hw1_tracked_lock}")', text)

    def test_monitor_skips_the_lock_only_when_it_cannot_configure(self):
        # idf.py monitor configures when project_description.json is missing,
        # and --config always reconfigures first.
        text = (REPO / "tools" / "build_board.sh").read_text()
        self.assertIn('-z "$CONFIG_NAME" && -f "$REPO/build-$BOARD/project_description.json"', text)

    def test_updater_locks_agree_across_targets(self):
        # updater/managed_components has no stamp: that is only safe while
        # every updater lock pins the same components.
        def pins(path):
            return [l for l in path.read_text().splitlines()
                    if not l.startswith("target:")]
        locks = sorted((REPO / "updater").glob("dependencies.lock.*"))
        self.assertGreaterEqual(len(locks), 2)
        for lock in locks[1:]:
            self.assertEqual(pins(lock), pins(locks[0]),
                             f"{lock.name} pins differ from {locks[0].name}; "
                             "stamp updater/managed_components like the root project")

    def test_stamp_and_locks_are_ignored(self):
        text = (REPO / "CMakeLists.txt").read_text()
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/.hw1-build/managed_components.stamp"', text)
        self.assertIn('lock="$repo/.hw1-build/checkout.lock"', LOCK_HELPER.read_text())
        self.assertIn("/.hw1-build/", (REPO / ".gitignore").read_text().splitlines())

    def test_build_scripts_take_the_checkout_lock_before_idf(self):
        for script in ("build_board.sh", "build_deployment.sh"):
            text = (REPO / "tools" / script).read_text()
            take = text.find("hw1_take_checkout_build_lock ")
            self.assertNotEqual(take, -1, script)
            self.assertLess(take, text.find("idf.py -C"), script)


class CheckoutBuildLock(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.holder = Path(self.temp.name) / "hold.sh"
        self.holder.write_text(f'''set -euo pipefail
. "{LOCK_HELPER}"
hw1_take_checkout_build_lock "{self.temp.name}" "$1" "$2"
echo locked
sleep "$3"
''')
        self.env = {k: v for k, v in os.environ.items()
                    if k != "HW1_CHECKOUT_BUILD_LOCK"}

    def start(self, target, what, seconds):
        # Own process group, so stop() also ends the sleep, which shares the
        # lock's open file just as idf.py and ninja do in a real build.
        build = subprocess.Popen(["bash", str(self.holder), target, what, str(seconds)],
                                 stdout=subprocess.PIPE, text=True, env=self.env,
                                 start_new_session=True)
        self.addCleanup(self.stop, build)
        self.assertEqual(build.stdout.readline().strip(), "locked")
        return build

    @staticmethod
    def stop(build):
        if build.poll() is None:
            os.killpg(build.pid, signal.SIGKILL)
        build.wait()
        build.stdout.close()

    def run_build(self, target, what, env=None):
        return subprocess.run(["bash", str(self.holder), target, what, "0"],
                              capture_output=True, text=True, env=env or self.env)

    def test_other_target_is_refused_while_a_build_runs(self):
        first = self.start("esp32p4", "p4x_eye", 30)
        second = self.run_build("esp32s3", "feathers3")
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("an esp32p4 build is running", second.stderr)
        self.assertIn("p4x_eye", second.stderr)
        # The lock goes with the process: no stale lock to clean up.
        self.stop(first)
        deadline = time.time() + 10
        while True:
            third = self.run_build("esp32s3", "feathers3")
            if third.returncode == 0 or time.time() > deadline:
                break
            time.sleep(0.2)
        self.assertEqual(third.returncode, 0, third.stderr)

    def test_same_target_builds_share_the_lock(self):
        self.start("esp32s3", "feathers3", 30)
        second = self.run_build("esp32s3", "xiao_s3")
        self.assertEqual(second.returncode, 0, second.stderr)
        # ... and a later build of another target is still refused.
        self.assertNotEqual(self.run_build("esp32", "feather_esp32_v2").returncode, 0)

    def test_nested_script_does_not_take_it_again(self):
        self.start("esp32p4", "p4x_eye", 30)
        env = dict(self.env, HW1_CHECKOUT_BUILD_LOCK="held by the parent")
        nested = self.run_build("esp32s3", "inner", env=env)
        self.assertEqual(nested.returncode, 0, nested.stderr)


if __name__ == "__main__":
    unittest.main()
