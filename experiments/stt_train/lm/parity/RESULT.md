# Cross-phrase LM context: measured, not adopted (2026-10-01)

context_eval.py (run15x5b step 8375, meeting-15x5b.lm, 4 AMI dev meetings x 150 consecutive
segments per mic, all speakers in time order):

| WER % | SDM | IHM |
|---|---:|---:|
| base (each phrase <s> ... </s>, the device) | 39.16 | 16.46 |
| open (no </s>) | 39.30 | 16.51 |
| ctx (previous decoded words) | 39.28 | 16.44 |
| ctx-open | 39.33 | 16.49 |
| oracle (previous reference words, no </s>) | 39.32 | 16.51 |

All within ~9 words of 5,232; even the oracle gains nothing, so a trigram history across phrase
boundaries has no headroom here. The C decoder change (DecodeStart) was verified bit-identical to
hw1lm.decode(context=, end_eos=) on 480 real decodes (context_parity.py/.cpp) and then reverted from
the firmware test tree. The Python options stay for experiments.
