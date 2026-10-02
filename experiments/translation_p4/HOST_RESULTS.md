# Standalone English to Spanish host screening

2026-09-29. Scope: English text in, Spanish text out, entirely offline after
setup. This is a Mac reference implementation for a possible P4 port. It is
not P4 firmware, a P4 speed test or proof that the translation engine fits.
The selected default is **Mozilla tiny with its full output vocabulary**
(shortlist disabled), after comparing two models and both shortlist settings.

## Reproducible reference

The runner uses the checked, unmodified Bergamot 0.4.9 WASM runtime and pinned
model assets. `screen.mjs` accepts a single text argument or the 20 synthetic
sentences in `fixtures.json`. It verifies asset hashes before inference,
blocks fetch/HTTP/socket APIs, and runs without a remote model registry.
The measured run also used the default network-restricted execution sandbox.
Model/runtime downloads are separate setup steps.

The [selected result](host-screen-selected.json) contains all input/output pairs, asset
identities, timings and memory observations. All 20 inputs produced output;
all 20 repeated outputs matched with translation caching disabled. There were
zero attempted network requests. This checks reproducibility and local
execution, not translation accuracy.

In a separate sequential selected run on this Apple M1, cold setup through the
first translation took 346 ms; warm repeats took 15-82 ms. Peak process RSS was
410,501,120 bytes (391.5 MiB), while WASM heap capacity reached 455,409,664 bytes
(434.3 MiB). The stock wrapper
configures 128 MiB of workspace. RSS includes Node and buffers, and WASM
capacity is not live allocated memory. None of these figures predicts a
custom native P4 implementation.

## Qualitative review of the selected default

| English input | Actual Spanish output | Review |
| --- | --- | --- |
| I would like a glass of water without ice. | Me gustaría un vaso de agua sin hielo. | Meaning preserved. |
| I do not want sugar in my coffee. | No quiero azúcar en mi café. | Negation preserved. |
| I ordered tea, not coffee. | Pedí té, no café. | Action and contrast preserved. |
| The ticket costs twenty-five dollars and fifty cents. | El billete cuesta veinticinco dólares y cincuenta centavos. | Numeric amounts and currency units preserved. |
| Our train leaves at 6:45 tomorrow morning. | Nuestro tren sale mañana a las 6:45. | Drops the explicit morning qualifier. |

The travel-ticket example uses *entradas* where *billetes* or *boletos* would
fit better. Most other punctuated samples preserve their basic meaning, with
formatting issues such as omitted opening question marks. These are assistant
qualitative judgments on a small synthetic set, not an independent human
evaluation, WER/BLEU benchmark or production accuracy claim.

The lowercase, unpunctuated variants degrade further, including turning a
request to repeat speech into a statement about not understanding someone's
ability to repeat it. Those fixtures remain evidence for later microphone
work; the first feature is standalone typed text.

## Why the default disables the shortlist

The [initial Mozilla run](host-screen.json) used the supplied word shortlist.
It produced the malformed verb *Peré* for *ordered* and *centés* for *cents*.
Using the full output vocabulary corrected both. It also removes the
3,347,104-byte shortlist asset from the inference inputs: model plus tokenizer
now total **17,966,218 bytes**. The unchanged upstream loader receives an empty
shortlist buffer; no downloaded runtime code was patched. This setting may
increase computation on P4 because all vocabulary entries are considered.

The second candidate was the pinned TranslatePsy-EuroNano Tiny English-to-
European pack with `##ES` prepended to each source sentence. Its supplied
shortlist produced [garbled output](host-screen-euronano.json) with the tested
runtime. Disabling that shortlist restored intelligibility, but the
[resulting outputs](host-screen-euronano-no-shortlist.json) still contained
malformed words, changed the ordering action and omitted Maria's name. It is
not selected. This is a finding about this runtime/model combination, not a
reproduction or refutation of the authors' evaluation with their own runtime.

The two preliminary shortlist-free diagnostic runs briefly overlapped. Their
timings are not a controlled comparison; the selected run above was performed
after those experiments finished. No quality percentage is claimed from this
small synthetic set.

## P4 implementation gate

The actual Mozilla binary was also parsed without executing it. Its 195
items and 16,907,062 model elements match the published architecture. Applying
the reviewed stock loader's embedding expansion yields 41,703,703 bytes of
logical item payload before working memory, already above 32 MiB. See
[the tensor map](model-tensors.json) and [port assessment](PORTING.md).
Seven parser boundary/corruption tests passed; model hash, parameter count,
output presence, repeat agreement and offline-report consistency were checked.

A P4 implementation needs a bounded native evaluator retaining compressed
embeddings, measured numerical agreement, and a concrete local storage plan.
No device or firmware was accessed or modified during this screening.
