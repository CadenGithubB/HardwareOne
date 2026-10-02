# R21 maintenance checks

This directory intentionally contains only the checks that still describe the
released R21 geometry.

- `unified-geometry-check.js` checks the sampled enclosure geometry, control
  travel, fasteners, wheel clearance, USB-cable clearance, and the R21 keeper.
  Run it with Node.js from this directory.
- `check_keeper_slot_poses.py` independently checks the keeper aperture over a
  grid of strip positions and yaw angles. It requires Python 3 and NumPy.

`clipper-6.4.2.js` and its accompanying license files are the local dependency
used by the JavaScript check. Per-revision export scripts, temporary viewer
experiments, generated logs, and previous CAD trials are retained in Git
history rather than kept beside the release files.
