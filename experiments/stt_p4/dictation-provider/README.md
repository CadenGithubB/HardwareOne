# Local dictation provider (draft)

An earlier route for fully local dictation: a provider that plugs the P4's
on-device STT into the shared dictation path, applied to the application as a
patch. Kept for reference; the shipped design lives in
`components/hardwareone/System_Dictation.cpp` and `System_STTLocal.cpp`.

- `local-dictation.patch` - the application change. `prepare.py` regenerates
  it, with edited copies of the affected files under `components/`, from the
  current production tree; it never modifies production sources.
- `local_provider.inc`, `local_provider_harness.cpp`, `test_local_provider.py`
  - the provider and its host test.

The full-size application files this patch produced are archived outside the
repository.
