#!/usr/bin/env python3
"""Level-robust onset (stt/stt_segmenter.h decision_pre_emphasis): unit cases U1-U16, mutants.

Ported from the speech repository's version 3.1 (tests/test_segmenter_decision.py) for
HardwareOne's copy of the header:
1. Builds stt_segmenter_decision_test.cpp against components/hardwareone/stt/stt_segmenter.h
   with -Werror and -fsanitize=address,undefined (UBSan reports are fatal) and runs every case.
2. Builds the option-0 diagnostics hash (U1) and the production hash with the noise controls
   off (U13) on their own and checks the golden values written in the test: option 0 is the
   1a51eac detector (the raw level only) bit for bit, and the onset at 97/150 is the speech
   repository's 12 Nov onset bit for bit. The speech repository recomputes both goldens from
   older headers in its own git history; HardwareOne has no such history, so only the goldens
   are checked here.
3. Builds header mutants that each revert ONE defect fix (the three HardwareOne judge defects,
   the gain-percentile graft, the three review fixes of 12 Nov, the two noise controls of
   13 Nov and their two review fixes) and checks that the case written for that fix fails on
   it (a test that passes on its mutant does not count).
No board or recordings are used.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]                      # components/hardwareone
SOURCE = HERE / "stt_segmenter_decision_test.cpp"
HEADER = COMPONENT / "stt/stt_segmenter.h"
POLICY = COMPONENT / "System_AudioVadPolicy.h"  # the header's only project include

# (name, case that must fail, text in stt_segmenter.h, replacement[, more (text, replacement) pairs])
# Same list as the speech repository's driver; the texts must match the header exactly once.
MUTANTS = [
    ("a: decision level from floor(sqrt) RMS", "U3",
     "        raw_e_=centered*kSegmentEnergyUnit/nn;",
     "        raw_e_=centered*kSegmentEnergyUnit/nn;\n"
     "        { const uint64_t q=integer_sqrt(static_cast<uint32_t>(std::min<uint64_t>(dec_e_/kSegmentEnergyUnit,0xffffffffu)));"
     " dec_e_=q*q*kSegmentEnergyUnit; }"),
    ("b: gain learned only during calibration (frozen after boot)", "U4",
     "        if(!active_)learn_gain();",
     "        if(!active_ && !calibrated_)learn_gain();"),
    ("c: quiet-cut carry compares raw energy with the decision threshold", "U6",
     "if((dec_votes && frame_energy_d(at)/(F*F)>=thr_d_) ||\n"
     "                   (raw_votes && frame_energy(at)*kSegmentEnergyUnit/(F*F)>=thr_r_))carry_voice_+=F;",
     "if((dec_votes || raw_votes) && frame_energy(at)*kSegmentEnergyUnit/(F*F)>=thr_d_)carry_voice_+=F;"),
    ("graft: gain percentile follows noise_percentile", "U9",
     "percentile_energy(idle_r_,idle_count_,kDecisionGainPercentile)",
     "percentile_energy(idle_r_,idle_count_,config_.noise_percentile)"),
    ("review: constant frames enter the gain window and a zero quartile maps to kSegmentGainMax", "U11",
     "        if((!calibrated_ || !active_) && raw_e_) {",
     "        if(!calibrated_ || !active_) {",
     [("        if(!d)return;\n", "        if(!d) { set_gain(kSegmentGainMax); return; }\n")]),
    ("review: constant frames enter the gain window (zero-quartile guard kept)", "U11",
     "        if((!calibrated_ || !active_) && raw_e_) {",
     "        if(!calibrated_ || !active_) {"),
    ("review: no re-seed after decision-only hard limits", "U12",
     "        if(config_.decision_pre_emphasis && segment_.end==SegmentEnd::HardLimit)after_hard_cut();\n",
     ""),
    ("gate: the pre-emphasised level votes whether or not it is modulated", "U14",
     "    bool pre_votes() const { return !config_.decision_modulation_ratio_pct || modulated_; }",
     "    bool pre_votes() const { return true; }"),
    ("hold: raw votes count before the raw level has latched", "U15",
     "    bool raw_votes_count() const { return !config_.decision_pre_hold_requires_raw || raw_heard_; }",
     "    bool raw_votes_count() const { return true; }"),
    ("hold review: the raw latch is kept through the whole hard-cut chain", "U15",
     "        if(!raw_onset_)raw_heard_=false;\n",
     ""),
    ("gate review: decision_modulated() reads the gate's ring state even while the gate is off", "U14",
     "    bool decision_modulated() const { return config_.decision_pre_emphasis && pre_votes(); }",
     "    bool decision_modulated() const { return modulated_; }"),
]


def include_tree(root, header_text):
    """The header and its one project include, laid out as in components/hardwareone."""
    (root / "stt").mkdir(parents=True, exist_ok=True)
    (root / "stt/stt_segmenter.h").write_text(header_text)
    shutil.copyfile(POLICY, root / POLICY.name)
    return root


def build(cxx, include, out, *extra, sanitize=True):
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run([cxx, *flags, *extra, "-I" + str(include), str(SOURCE), "-o", str(out)], check=True, timeout=300)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hw1-stt-segmenter-decision-") as tmp:
        tmp = Path(tmp)
        original = HEADER.read_text()

        # 1. All cases on the real header, under ASan/UBSan.
        binary = tmp / "decision"
        build(args.cxx, COMPONENT, binary)
        subprocess.run([str(binary)], check=True, timeout=600)

        # 2. The two golden hashes, each from its own build.
        for define, constant, what in (("-DSEGMENTER_DIAG_HASH_ONLY", "kGoldenDiagHash", "option-0 diagnostics hash (1a51eac)"),
                                       ("-DSEGMENTER_PROD_HASH_ONLY", "kGoldenProdHash", "option-on hash, noise controls off")):
            golden = int(re.search(constant + r"=(\d+)ull", SOURCE.read_text()).group(1))
            out = tmp / ("hash" + define.lower())
            build(args.cxx, COMPONENT, out, define, "-Wno-unused-function", "-Wno-unused-const-variable", sanitize=False)
            value = int(subprocess.run([str(out)], capture_output=True, text=True, check=True, timeout=300).stdout)
            assert value == golden, (what, value, golden)
            print(f"{what}: {value} (golden)")

        # 3. Each defect fix is load-bearing for its case.
        for name, case, old, new, *more in MUTANTS:
            text = original
            for o, n in [(old, new), *(more[0] if more else [])]:
                assert text.count(o) == 1, f"mutant '{name}': text not found exactly once in stt_segmenter.h: {o!r}"
                text = text.replace(o, n)
            mutant = tmp / "mutant"
            if mutant.exists():
                shutil.rmtree(mutant)
            include_tree(mutant, text)
            out = tmp / "mutant_test"
            build(args.cxx, mutant, out, sanitize=False)
            r = subprocess.run([str(out), case], capture_output=True, text=True, timeout=600)
            assert r.returncode != 0, f"mutant '{name}' was NOT detected by {case}:\n{r.stdout}"
            failed = [line.strip() for line in r.stdout.splitlines() if "FAILED " in line]
            print(f"mutant {name}: detected by {case} ({'; '.join(failed)})")

    print("segmenter decision checks passed")


if __name__ == "__main__":
    main()
