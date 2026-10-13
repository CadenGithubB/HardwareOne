# HardwareOne STT training kit (Ubuntu + NVIDIA GPU)

Everything needed to continue training the P4's speech model on a CUDA PC, on one
exFAT SSD (shown here as `<drive>`, its volume label):

```
<drive>/
  stt/                 data: stores (training audio), work (checkpoints, results), models, noise
  hw1-stt-kit/         this kit: code snapshot + scripts
```

## On the Ubuntu PC

Ubuntu 22.04 or newer. The one-time setup needs internet (about 3 GB of downloads).

1. **NVIDIA driver.** In a terminal, `nvidia-smi` should list the RTX A4000. If the
   command is not found:
   ```
   sudo ubuntu-drivers install
   sudo reboot
   ```
   If Secure Boot is on, the installer asks you to choose a password. After the reboot,
   a blue screen appears: choose "Enroll MOK", then enter that password.
2. **Plug in the SSD.** Ubuntu mounts it at `/media/$USER/<drive>`
   (`ls /media/$USER/<drive>` shows `stt` and `hw1-stt-kit`).
3. **One-time setup:**
   ```
   sudo apt-get install -y python3-venv python3-dev build-essential
   cd /media/$USER/<drive>/hw1-stt-kit
   bash setup_ubuntu.sh
   ```
   It prints the GPU, then ends with `setup done`.
4. **Start the run:**
   ```
   cd /media/$USER/<drive>/hw1-stt-kit
   nohup bash run_continuation.sh > /dev/null 2>&1 &
   tail -f ../stt/logs/continuation.log
   ```
   Ctrl+C stops watching the log, not the run. The run keeps the PC from going to sleep and
   keeps going if you log out, but don't shut the PC down. To check on it later, run
   `tail ../stt/logs/continuation.log` from the kit folder, or `nvidia-smi` to see the GPU
   busy. If the PC restarts anyway, run the `nohup` line again: it resumes where it
   stopped.
5. **When the log says `=== done`**, eject <drive> in the Files app (or
   `udisksctl unmount -b "$(findmnt -no SOURCE /media/$USER/<drive>)"`) and bring the
   SSD back to the Mac.

`STT_ROOT` (the `stt` folder) and the Python (`PY`) can be overridden if the SSD is
mounted elsewhere: `STT_ROOT=/mnt/ssd/stt bash run_continuation.sh`.

## What the run does

- Filters the new People's Speech shards (drops clips whose transcript doesn't match the
  audio), then runs a 60-step speed check to pick bf16 or fp32 and the batch split.
- Starts from `stt/work/general/quartznet15x5/best` (the general model, Broad 23.11).
- `--mix general2`: People's Speech 30% (old and new shards), Common Voice 12%,
  AMI 25%, LibriSpeech 15%, VoxPopuli 18%.
- Background noise from MUSAN and DEMAND everyday environments; 4% of items are pure
  noise with an empty transcript (teaches the model to write nothing for noise).
- Simulated glasses mic channel on 30% of items.
- No rollback: every evaluation is saved and the best one is kept; peak learning rate
  1e-5 with warmup and a cosine fade; 500 h of audio.
- Results: `stt/work/general2/quartznet15x5/` (`best/`, `evals/`, `train.jsonl`) and
  `hw1-stt-kit/experiments/stt_train_15x5/BENCHMARK.md`.

The P4 export and deployment stay on the Mac (they need the ESP-IDF tool chain).
