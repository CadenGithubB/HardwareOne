"""Articulated hand and robe V1 use deterministic real mesh geometry."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js

CHECK_COUNT = 89
PASS_RESULT = f"HAND_RIG_RESULT PASS {CHECK_COUNT}"


class HandRigTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_geometry_and_canvas_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), timeout=40)
        lines = result.stdout.splitlines()
        self.assertIn(PASS_RESULT, lines, result.stdout + result.stderr)
        self.assertEqual(CHECK_COUNT, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_inward_surface_winding(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--invert-hand-winding"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: palm exterior triangles wind outward rather than displaying inner backs", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_frozen_mesh_deformation(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--freeze-hand-motion"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: animation changes actual 3D vertex positions instead of swapping images", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_left_hand_mislabeled_as_right(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--mirror-hand-chirality"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: hand has right-handed anatomical chirality in three dimensions", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_casting_flourish_while_idle(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--idle-replays-cast"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: advancing idle clocks never replay a casting flourish in any grip or style", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_shared_palm_thrust_for_finger_guns(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--shared-palm-thrust"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Arcane reaches away while Finger Guns recoils toward the camera", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_arcane_gather_palm_flip(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-arcane-palm-flip"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Arcane palm stays away and dorsal surface stays toward camera throughout all grip cycles", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_sideways_default_handle(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--omit-handle-roll"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: default Handle projects its held-item axis upright in the player view", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_hooked_forearm_centerline(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--hook-forearm-centerline"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Handle axial rotation preserves every authored forearm centerline point", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_lateral_forearm_entry(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-lateral-forearm"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Handle forearm projects below the wrist rather than far across the screen", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_rigid_handle_release(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-rigid-handle-release"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Handle held-item axis tilts naturally forward at maximum extension", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_groundward_finger_guns(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-groundward-finger-guns"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Finger Guns projected index aim passes near the crosshair at preview and fullscreen sizes", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_exposed_short_sleeve_end(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-short-sleeve"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: proximal sleeve entry stays beyond the player viewport in all styles grips and phases", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_curled_middle_finger(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-curled-middle-finger"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Finger Guns keeps index and middle extended with ring and little tucked", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_loose_ring_and_little_fingers(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-loose-finger-curl"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Finger Guns ring and little concentrate their main fold at the second knuckle", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_palm_first_ring_and_little_fold(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-palm-first-finger-curl"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Finger Guns ring and little proximal bones stay straight out from the palm like the aiming fingers", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_uncurled_distal_ring_and_little_fingers(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--unfold-returning-fingertips"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Finger Guns ring and little distal bones return toward the palm after the PIP fold", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_base_driven_thumb_press(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-base-swing-thumb"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Finger Guns thumb press keeps its buried base and whole-thumb swing restrained", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_handle_inheriting_style_recoil(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-handle-style-recoil"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Handle release moves the actual wrist and palm farther from the camera in both styles", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_thumb_sticking_out_of_palm(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-palm-out-thumb"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: reference thumb silhouette keeps an open raised gap above the index throughout the press", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_tilted_finger_guns_palm(self) -> None:
        result = run_js(Path(__file__).with_name("hand_rig.js"), ["--restore-tilted-finger-guns"], timeout=40)
        self.assertNotIn(PASS_RESULT, result.stdout.splitlines())
        self.assertIn("Error: Finger Guns palm radial axis stays upright through the entire casting cycle", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
