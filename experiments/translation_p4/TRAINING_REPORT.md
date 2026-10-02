# Can we train the offline English -> Spanish translator to improve it?

Assessment: 2026-09-29, America/New_York. Scope: a standalone text translator,
with speech recognition feeding English text into it later.

**Yes: there is a credible fine-tuning path for the selected Bergamot tiny
model family.** We can adapt a floating-point checkpoint to better handle
everyday conversation and travel, then export compact int8 weights again.
Keeping the architecture and vocabulary fixed should retain approximately the
current deployment size. Actual improvement must be demonstrated on unseen
sentences; successful training alone would not establish better translation.

**Status: report only. Translation training is on hold while the user's
15×5 speech-to-text training is running.** No translation training, training
build, dataset download or automatic follow-up job was started for this
report. The existing STT training was not accessed or interrupted. Earlier
host inference results below predate this resource hold.

## Selected reference and trainable checkpoint

The current inference reference is **Mozilla/Bergamot tiny English -> Spanish,
using the full output vocabulary**. Its inspected model has 16,907,062
parameters, a six-layer Transformer encoder, two SSRU decoder layers,
256-wide embeddings and a shared 32,000-token vocabulary. Its int8 model is
17,140,755 bytes; model plus tokenizer total 17,966,218 bytes. See the local
[host results](HOST_RESULTS.md) and [tensor inspection](PORTING.md).

The original Bergamot project publishes a training-format **FP32 `model.npz`**
for `enes.student.tiny11`, plus its shared vocabulary. Both URLs returned
HTTP 200 to lightweight HEAD checks during this investigation. The directory
lists the checkpoint at approximately 65 MB; weights were not downloaded.
The published tiny11 architecture matches the dimensions above.
[Upstream download script](https://github.com/browsermt/students/blob/master/esen/download-models.sh),
[model description](https://github.com/browsermt/students/blob/master/esen/README.md),
[checkpoint directory](https://data.statmt.org/romang/bergamot/models/esen/enes.student.tiny11/).

| Item | Evidence and remaining check |
| --- | --- |
| Tested int8 reference | SHA-256 `fa7460037a3163e03fe1d23602f964bff2331da6ee813637e092ddf37156ef53`; locally verified and screened. |
| Proposed training checkpoint | [Original tiny11 FP32 NPZ](https://data.statmt.org/romang/bergamot/models/esen/enes.student.tiny11/model.npz); accessible, but tensor contents and export equivalence remain unverified. |
| Proposed tokenizer | [Original shared vocabulary](https://data.statmt.org/romang/bergamot/models/esen/vocab.esen.spm); accessible, but must be hashed and compared with the tested tokenizer. |
| First prerequisite | Pin the training source/build, inspect the checkpoint and vocabulary, and reproduce a comparable unmodified int8 baseline before adapting weights. |

Matching the model name and architecture does not prove the FP32 checkpoint
exports to our exact tested binary. If they differ, treat the checkpoint as
a separate baseline and evaluate it before choosing it. Reconstructing
training weights from the lossy deployment binary is unnecessary while a
suitable FP32 source is available.

Native Marian supports continued training of an existing NPZ model and
pretrained initialization. Its documentation recommends a lower learning rate
and more frequent validation for smaller adaptation datasets.
[Marian fine-tuning documentation](https://marian-nmt.github.io/docs/#fine-tuning).

Preserve provenance by artifact: the original tiny11 checkpoint's catalog
declares **CC-BY-SA-4.0**, while Mozilla's model distribution declares
**MPL-2.0**. A training run starting from the original NPZ must record that
original source and license rather than silently assigning Mozilla's label.
[Tiny11 catalog](https://github.com/browsermt/students/blob/master/esen/enes.student.tiny11/catalog-entry.yml),
[Bergamot license](https://github.com/browsermt/students/blob/master/LICENSE.md),
[Mozilla model distribution](https://github.com/mozilla/translations).

## What training could improve

The small local screen suggests concrete targets:

| Observed behavior | Target for training and evaluation |
| --- | --- |
| "Our train leaves at 6:45 tomorrow morning." becomes "Nuestro tren sale mañana a las 6:45." | Preserve the morning qualifier and other time details. |
| Travel tickets become "entradas" in one sample. | Choose context-appropriate transport vocabulary, allowing valid regional variants such as *billetes* and *boletos*. |
| Some questions omit opening ¿; one greeting gains a stray hyphen. | Improve question punctuation and conversational formatting without changing meaning. |
| Lowercase, unpunctuated speech-like input can change request intent. | Later robustness work using representative English STT output; typed, punctuated input remains the first milestone. |

These are assistant judgments on 20 synthetic examples, not an independent
accuracy benchmark. The same examples have already influenced model and
decoder selection, so they are **development/regression data, not an unseen
final test**.

Runtime configuration also matters. Disabling the shortlist changed malformed
"Peré" to "Pedí" and "centés" to "centavos" without changing any model weights.
Those improvements already belong to the baseline. Training should address
remaining model weaknesses after runtime correctness is established.
[Recorded outputs](host-screen-selected.json).

## Recommended training approach

Start with **targeted fine-tuning of the existing tiny architecture**. Keep
the tokenizer, dimensions and decoding configuration fixed so we can attribute
changes to training and preserve the expected weight footprint. Do not begin
by training an entirely new translator.

The following dataset sizes are proposed pilot targets, not established
minimum requirements: roughly 5,000-20,000 clean English/Spanish adaptation
pairs, mixed with 50,000-200,000 suitable general translation pairs to limit
loss of broader ability. Cover requests, directions, travel, amounts, names,
dates, negation and conversational phrasing. Record source, usage terms,
cleanup and version; remove duplicates and split related documents and
paraphrase families together to avoid leakage. Favor faithful Spanish over
forcing a single regional wording.

A stronger translator could generate additional training pairs through
**distillation**: the small model learns from a larger model's translations.
This is an established component of Bergamot's student-training approach.
Filter and review synthetic translations; teacher output is not independent
ground truth. Teacher choice and any external computation would be a later
decision, not part of this report.
[Bergamot training recipe](https://github.com/browsermt/students/tree/master/train-student),
[sequence-level distillation research](https://aclanthology.org/D16-1139/).

After ordinary adaptation, test a short quantization-aware stage that emulates
8-bit matrix arithmetic, then recalibrate and export the deployment model.
Upstream warns that this stage can improve quickly and then degrade, so use
frequent validation and retain the best checkpoint. Judge the **final int8
output**, not just the floating-point training result.
[Bergamot quantization-aware guidance](https://github.com/browsermt/students/blob/master/train-student/finetune/README.md),
[Mozilla pipeline stages](https://github.com/mozilla/translations/blob/main/docs/training/pipeline-steps.md).

## How we would demonstrate that it is better

Before training, freeze a new bilingual-reviewed test set: a practical initial
target is about 500 representative sentences, including at least 100 challenge
cases for names, numbers, negation, time qualifiers and request intent. Keep
a separate general-domain test and a separate development set for checkpoint
selection. Keep future STT-style inputs as a separately reported group.

Compare the original int8 baseline, its unadapted FP32 training source, the
adapted FP32 checkpoint and the final adapted int8 export under matched input
processing and greedy decoding. Record corpus chrF/BLEU scores with tool
versions, blind bilingual meaning judgments, and explicit counts of omitted
or changed facts. Report uncertainty where the test size supports it.
Automated overlap scores alone cannot establish conversational reliability.
[SacreBLEU evaluation tooling](https://github.com/mjpost/sacrebleu).

Promote a trained model only if it improves unseen practical translations,
does not materially regress on the general test or critical meaning checks,
and passes size/runtime verification after export. Define the scoring and
acceptance criteria before examining final-test results. Retain the original
model and hashes for reproducible comparisons. No gain percentage is justified
before this experiment.

## Compute needs and the P4 limit

Training would run on a workstation; the P4 would receive the resulting
inference weights. Marian's fork contains Apple Accelerate and ARM CPU build
paths, but that does not establish a working training/quantization pipeline
or Apple GPU acceleration on this M1. After the STT resource hold is explicitly
lifted, a bounded pilot should measure build compatibility, peak memory and
examples per second before estimating a longer run.
[Marian build configuration](https://github.com/browsermt/marian-dev/blob/master/CMakeLists.txt).

For scale only, 16,907,062 parameters require about 67.6 MB as FP32 values.
FP32 weights, gradients and two Adam moment arrays total approximately
270.5 MB (258 MiB). This arithmetic excludes activations, batches, workspaces,
temporary copies and the training program, so it is **not a total RAM estimate**.
No training duration or machine-capacity claim has been measured.

Ordinary fine-tuning preserves approximately the 17.14 MB int8 weight size;
it does not solve these independently established P4 problems:

- The stock loader's embedding expansion gives 41,703,703 bytes of logical
  tensor payload before working memory, exceeding 32 MiB PSRAM. A native
  runtime retaining compressed embeddings remains necessary.
- Model plus tokenizer exceed the board's total 16 MiB flash. Local storage
  or a smaller model still needs qualification.
- No standalone P4 translation latency, peak memory or numerical parity has
  been measured. Better host translations do not establish onboard operation.

See the [P4 port assessment](PORTING.md). Distilling a smaller architecture or
vocabulary is a possible second research path if the current size proves
impractical, but it requires fresh training and evaluation and may reduce
quality. It is a larger experiment than adapting the existing translator.

## Deferred execution sequence

1. Once explicitly resumed, verify the FP32 checkpoint, vocabulary and
   reproducible baseline; keep original assets immutable.
2. Prepare versioned training/development/test splits and freeze evaluation
   criteria. Verify the native training toolchain with a bounded pilot.
3. Run a small adaptation experiment with frequent validation and early
   stopping; expand only if measured quality and resource use justify it.
4. Calibrate/export int8, evaluate the frozen tests, and record both gains
   and regressions. Keep the baseline if adaptation does not improve it.
5. Advance a successful model to the separate bounded P4 runtime/storage
   experiment; connect English STT output only after standalone text works.

This is a proposed future experiment, not a queued job. No automatic action
will start when the 15×5 speech training finishes.
