#!/usr/bin/env python3
"""Fine-tune QuartzNet15x5 with the run2 (QuartzNet5x5) recipe, like for like.

Copy of experiments/stt_train/train.py with these deliberate differences:
  * TRAIN_MIX is run2's mix (AMI SDM/IHM + LibriSpeech clean-100), not run3's.
  * The run length is measured in audio hours seen (run2 saw ~175 h) instead
    of wall-clock time, because the 15x5 is ~3x slower per audio second. The
    cosine schedule follows max(audio fraction, wall-clock fraction), so a
    slow run still decays fully before the deadline.
  * Dev evaluations are spaced by audio hours (run2's 30-min cadence was
    ~26 h of audio; its first eval came after ~12.6 h).
  * Gradient accumulation: --max-seconds / --max-items give the effective
    batch per optimizer step (run2: 110 s / 32 items) and --accum splits it
    into micro-batches so the bigger model fits in memory. Same optimizer
    steps, same batch, same LR as run2.
Options added 2026-09-30 for continued training, all off by default:
--optimizer novograd (NeMo's recipe), --mix broad (+VoxPopuli), --dev g2 and
--g2-prob (simulated glasses mic channel, see lc3codec.py).
Everything else (augmentation, bucketed batch shapes, AdamW 2e-5, warmup 300,
frozen BN stats, step-0 eval, CPU eval copy, rollback guard, memory restart)
is unchanged.
"""
import argparse
import copy
import ctypes
import datetime as dt
import functools
import os
import json
import math
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

from common import QuartzNet, load_checkpoint, save_checkpoint as _save_checkpoint, BLANK
from citrinet import Citrinet, is_citrinet, load_vocab


def save_checkpoint(model, template, config, directory):
    _save_checkpoint(model, template, config, directory)
    if hasattr(model, 'tokenizer'):  # Citrinet: keep its WordPiece vocabulary beside the weights
        (Path(directory) / 'vocab.txt').write_text('\n'.join(model.tokenizer.vocab) + '\n')
from data import Store, Augmenter, TrainSet, MixedBatchSampler, collate
from evaluate import evaluate_stores

TRAIN_MIX = [  # run2's mix: (store, weight, far_field)
    ('ami-sdm-train', 0.35, True),
    ('ami-ihm-train', 0.35, False),
    ('librispeech-train-clean-100', 0.30, False),
]
# --mix broad: run2's sources plus VoxPopuli (parliament speech, many accents;
# already on disk), keeping LibriSpeech near a quarter against clean regression.
BROAD_MIX = [
    ('ami-sdm-train', 0.30, True),
    ('ami-ihm-train', 0.25, False),
    ('librispeech-train-clean-100', 0.25, False),
    ('voxpopuli-train', 0.20, False),
]
# --mix expanded (2026-10-01): adds People's Speech (mostly public meetings and
# interviews; filtered with keep.json), 100 h more LibriSpeech speakers
# (clean-360 subset) and 4 more VoxPopuli shards. AMI stays at 45%, the only
# real meeting-room audio; read speech ~15% to hold clean accuracy.
EXPANDED_MIX = [
    ('ami-sdm-train', 0.25, True),
    ('ami-ihm-train', 0.20, False),
    ('peoples-clean-train', 0.25, False),
    ('librispeech-train-clean-100', 0.08, False),
    ('librispeech-train-clean-360-p28', 0.07, False),
    ('voxpopuli-train', 0.08, False),
    ('voxpopuli-train-b', 0.07, False),
]
MIXES = {'run2': TRAIN_MIX, 'broad': BROAD_MIX, 'expanded': EXPANDED_MIX}
DEV_SETS = [('ami-sdm-validation', 400), ('ami-ihm-validation', 400), ('librispeech-dev-clean', 400)]
# --dev g2 adds the same AMI subsets passed through the simulated glasses
# channel (make_g2_stores.py), so selection also rewards glasses robustness.
G2_DEV_SETS = DEV_SETS + [('ami-sdm-validation-g2', 400), ('ami-ihm-validation-g2', 400)]


class NovoGrad(torch.optim.Optimizer):
    """NovoGrad as in NeMo (Ginsburg et al. 2019), QuartzNet's own optimizer.

    Per tensor: the second moment is one scalar (EMA of the squared gradient
    norm), the gradient is normalised by it, weight decay is added to the
    normalised gradient, then momentum. With grad_averaging=False (NeMo's
    default) the momentum buffer is a running sum, so lr is the per-tensor step
    size scale; NeMo's fine-tuning examples use lr ~1e-3 with this.
    """

    def __init__(self, params, lr=1e-3, betas=(0.95, 0.5), eps=1e-8, weight_decay=1e-3,
                 grad_averaging=False):
        super().__init__(params, dict(lr=lr, betas=betas, eps=eps, weight_decay=weight_decay,
                                      grad_averaging=grad_averaging))

    @torch.no_grad()
    def step(self, closure=None):
        for group in self.param_groups:
            beta1, beta2 = group['betas']
            for p in group['params']:
                if p.grad is None:
                    continue
                grad = p.grad
                state = self.state[p]
                norm = grad.square().sum()
                if not state:
                    state['exp_avg'] = torch.zeros_like(p)
                    state['exp_avg_sq'] = norm.clone()
                else:
                    state['exp_avg_sq'].mul_(beta2).add_(norm, alpha=1 - beta2)
                g = grad / (state['exp_avg_sq'].sqrt() + group['eps'])
                if group['weight_decay']:
                    g.add_(p, alpha=group['weight_decay'])
                if group['grad_averaging']:
                    g.mul_(1 - beta1)
                state['exp_avg'].mul_(beta1).add_(g)
                p.add_(state['exp_avg'], alpha=-group['lr'])


RESTART_EXIT = 75  # run_15x5.sh restarts on this without shrinking batches
MEMORY_EXIT = 3    # benchmark exceeded the footprint limit


def footprint_gb():
    """Physical footprint incl. compressed/swapped pages (what top's MEM shows)."""
    try:
        buf = ctypes.create_string_buffer(256)
        libproc = ctypes.CDLL('/usr/lib/libproc.dylib')
        if libproc.proc_pid_rusage(os.getpid(), 2, buf) != 0:  # RUSAGE_INFO_V2
            return 0.0
        return int.from_bytes(buf.raw[72:80], 'little') / 2**30  # ri_phys_footprint
    except OSError:
        return 0.0


def parse_deadline(text):
    now = dt.datetime.now()
    hh, mm = map(int, text.split(':'))
    deadline = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
    if deadline <= now:
        deadline += dt.timedelta(days=1)
    return deadline.timestamp()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--init', required=True, help='NeMo-format checkpoint dir to start from')
    p.add_argument('--out', required=True)
    p.add_argument('--deadline', required=True, help='HH:MM local time to stop at the latest')
    p.add_argument('--target-hours', type=float, default=175.0,
                   help='stop after this many hours of (augmented) training audio; run2 saw ~175 h')
    p.add_argument('--lr', type=float, default=2e-5)
    p.add_argument('--optimizer', choices=['adamw', 'novograd'], default='adamw',
                   help='novograd: NeMo-style, use with --lr ~1e-3')
    p.add_argument('--betas', type=float, nargs=2, help='default: AdamW (0.9, 0.999), NovoGrad (0.95, 0.5)')
    p.add_argument('--weight-decay', type=float, default=1e-3)
    p.add_argument('--mix', choices=sorted(MIXES), default='run2')
    p.add_argument('--dev', choices=['default', 'g2'], default='default')
    p.add_argument('--g2-prob', type=float, default=0.0,
                   help='fraction of training items passed through the simulated glasses channel '
                        '(slow AGC + LC3 16k/10ms/32kbps with occasional PLC)')
    p.add_argument('--warmup', type=int, default=300)
    p.add_argument('--max-seconds', type=float, default=110.0,
                   help='padded audio seconds per optimizer step (split over --accum micro-batches)')
    p.add_argument('--max-items', type=int, default=32, help='items per optimizer step')
    p.add_argument('--accum', type=int, default=1, help='micro-batches per optimizer step')
    p.add_argument('--workers', type=int, default=5)
    p.add_argument('--eval-hours', type=float, default=26.2, help='audio hours between dev evals')
    p.add_argument('--first-eval-hours', type=float, default=12.6)
    p.add_argument('--save-steps', type=int, default=250)
    p.add_argument('--max-footprint-gb', type=float, default=6.0,
                   help='save and exit with code 75 (restart) above this memory footprint')
    p.add_argument('--patience', type=int, default=2, help='worse evals before rolling back to best')
    p.add_argument('--benchmark-steps', type=int, default=0, help='run N steps, report speed, exit')
    p.add_argument('--device', default='mps' if torch.backends.mps.is_available() else 'cpu',
                   help='cpu only for tiny smoke tests')
    a = p.parse_args()
    train_mix = MIXES[a.mix]
    dev_sets = G2_DEV_SETS if a.dev == 'g2' else DEV_SETS
    micro_seconds = a.max_seconds / a.accum
    micro_items = max(1, a.max_items // a.accum)

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    log = open(out / 'train.jsonl', 'a')

    def emit(**kw):
        kw['time'] = dt.datetime.now().strftime('%H:%M:%S')
        line = json.dumps(kw)
        print(line, flush=True)
        log.write(line + '\n')
        log.flush()

    device = torch.device(a.device)
    config, template = load_checkpoint(a.init)
    if is_citrinet(config):
        model = Citrinet(config, load_vocab(a.init)).load_nemo(template).to(device)
        # The tokenizer alone goes to the DataLoader workers (never the MPS model).
        encoder, blank, reduction = model.tokenizer.encode, model.blank, 8
    else:
        model = QuartzNet(config).load_nemo(template).to(device)
        encoder, blank, reduction = None, BLANK, 2
    if a.optimizer == 'novograd':
        optimizer = NovoGrad(model.parameters(), lr=a.lr, betas=tuple(a.betas or (0.95, 0.5)),
                             weight_decay=a.weight_decay)
    else:
        optimizer = torch.optim.AdamW(model.parameters(), lr=a.lr, betas=tuple(a.betas or (0.9, 0.999)),
                                      weight_decay=a.weight_decay)

    step, best = 0, None
    # Rollback guard: after `patience` evals worse than the best, restore the
    # best weights and halve the learning rate instead of drifting further.
    guard = {'bad': 0, 'lr_scale': 1.0, 'audio_seconds': 0.0}
    resume = out / 'resume.pt'
    if resume.exists() and not a.benchmark_steps:
        blob = torch.load(resume, weights_only=False, map_location='cpu')
        model.load_state_dict(blob['model'])
        optimizer.load_state_dict(blob['optimizer'])
        step, best = blob['step'], blob.get('best')
        guard.update(blob.get('guard', {}))
        emit(event='resumed', step=step, best=best, guard=guard)
    emit(event='config', accum=a.accum, micro_seconds=micro_seconds, micro_items=micro_items,
         target_hours=a.target_hours, lr=a.lr, optimizer=a.optimizer, betas=a.betas,
         weight_decay=a.weight_decay, mix=train_mix, dev=dev_sets, g2_prob=a.g2_prob)

    def save_resume():
        torch.save({'model': model.state_dict(), 'optimizer': optimizer.state_dict(),
                    'step': step, 'best': best, 'guard': guard}, out / 'resume.pt.tmp')
        (out / 'resume.pt.tmp').replace(resume)

    augmenter = Augmenter(Store('musan-noise'), Store('musan-speech'), Store('rirs'), g2_prob=a.g2_prob)
    stores = [Store(name) for name, _, _ in train_mix]
    dataset = TrainSet(stores, [f for _, _, f in train_mix], template, augmenter, seed=step + 1,
                       **({'encoder': encoder} if encoder else {}))
    total_steps = (a.benchmark_steps * a.accum) or 10 ** 9
    sampler = MixedBatchSampler(stores, [w for _, w, _ in train_mix], micro_seconds, micro_items,
                                total_steps, seed=step + 17)
    loader = torch.utils.data.DataLoader(dataset, batch_sampler=sampler,
                                         collate_fn=functools.partial(collate, max_seconds=micro_seconds,
                                                                      max_items=micro_items, reduction=reduction),
                                         num_workers=a.workers, persistent_workers=True,
                                         prefetch_factor=4)

    start = time.time()
    deadline = parse_deadline(a.deadline) if not a.benchmark_steps else start + 1e9
    target = a.target_hours * 3600
    if not a.benchmark_steps:
        # The schedule spans the whole run, not each process: memory restarts
        # must not reset the LR to its peak.
        first = getattr(os.stat(out / 'train.jsonl'), 'st_birthtime', start)
        start = guard.setdefault('sched_start', min(first, start))
    span = max(deadline - start, 1.0)
    # Score the starting point first so "best" can never be worse than it.
    if best is None and not a.benchmark_steps:
        best = run_eval(model, template, config, out, step, best, emit, device, guard, a.patience, dev_sets)
        guard['last_eval_audio'] = -(a.eval_hours - a.first_eval_hours) * 3600
        save_resume()
    guard.setdefault('last_eval_audio', 0.0)
    process_audio, t_process = 0.0, time.time()
    window_loss, window_n, t_window = 0.0, 0, time.time()
    micro, micro_loss = 0, 0.0
    train_mode(model)

    for feats, lengths, labels, label_lengths in loader:
        if micro == 0:
            now = time.time()
            if now >= deadline:
                emit(event='deadline', step=step, audio_hours=round(guard['audio_seconds'] / 3600, 2))
                break
            if guard['audio_seconds'] >= target and not a.benchmark_steps:
                emit(event='target_reached', step=step, audio_hours=round(guard['audio_seconds'] / 3600, 2))
                break
            # Linear warmup, then cosine decay over the audio budget (or the
            # remaining wall-clock time, whichever runs out first).
            progress = min(1.0, max(guard['audio_seconds'] / target, (now - start) / span))
            lr = a.lr * guard['lr_scale'] * min(1.0, (step + 1) / a.warmup) * \
                (0.05 + 0.95 * 0.5 * (1 + math.cos(math.pi * progress)))
            for g in optimizer.param_groups:
                g['lr'] = lr
            optimizer.zero_grad(set_to_none=True)
            micro_loss = 0.0

        log_probs, out_lengths = model(feats.to(device), lengths.to(device))
        log_probs = F.log_softmax(log_probs.float(), dim=-1)
        loss = F.ctc_loss(log_probs.transpose(0, 1).cpu(), labels, out_lengths.cpu(), label_lengths,
                          blank=blank, reduction='mean', zero_infinity=True)
        (loss / a.accum).backward()
        micro_loss += loss.item() / a.accum
        seconds = float(lengths.sum()) * 0.01
        guard['audio_seconds'] += seconds
        process_audio += seconds
        micro += 1
        if micro < a.accum:
            continue
        micro = 0
        torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
        optimizer.step()
        step += 1
        window_loss += micro_loss
        window_n += 1

        if a.benchmark_steps and step % 10 == 0 and footprint_gb() > a.max_footprint_gb:
            emit(event='benchmark_memory', step=step, mem_gb=round(footprint_gb(), 2))
            log.close()
            os._exit(MEMORY_EXIT)

        if step % 50 == 0:
            elapsed = time.time() - t_window
            emit(event='train', step=step, loss=round(window_loss / window_n, 4), lr=f'{lr:.2e}',
                 audio_hours=round(guard['audio_seconds'] / 3600, 2),
                 steps_per_s=round(window_n / elapsed, 3), batch=int(len(lengths)),
                 mem_gb=round(footprint_gb(), 2))
            window_loss, window_n, t_window = 0.0, 0, time.time()
            if not a.benchmark_steps and footprint_gb() > a.max_footprint_gb:
                save_resume()
                emit(event='memory_restart', step=step, mem_gb=round(footprint_gb(), 2))
                log.close()
                os._exit(RESTART_EXIT)

        if a.benchmark_steps:
            if step >= a.benchmark_steps:
                rate = process_audio / (time.time() - t_process)
                emit(event='benchmark', accum=a.accum, audio_seconds_per_second=round(rate, 2),
                     mem_gb=round(footprint_gb(), 2),
                     peak_mps_mb=round(torch.mps.driver_allocated_memory() / 2**20) if device.type == 'mps' else None)
                log.close()
                os._exit(0)  # skip slow DataLoader worker teardown
            continue

        if step % a.save_steps == 0:
            save_resume()

        if guard['audio_seconds'] - guard['last_eval_audio'] >= a.eval_hours * 3600:
            best = run_eval(model, template, config, out, step, best, emit, device, guard, a.patience, dev_sets)
            guard['last_eval_audio'] = guard['audio_seconds']
            save_resume()
            train_mode(model)

    save_checkpoint(model, template, config, out / 'last')
    best = run_eval(model, template, config, out, step, best, emit, device, guard, a.patience, dev_sets)
    save_resume()
    emit(event='finished', step=step, best=best, audio_hours=round(guard['audio_seconds'] / 3600, 2))
    log.close()
    os._exit(0)


def train_mode(model):
    """Train weights but keep the pretrained BatchNorm statistics: augmented,
    padded fine-tuning batches would otherwise drag them off the real-audio
    distribution."""
    model.train()
    for m in model.modules():
        if isinstance(m, torch.nn.modules.batchnorm._BatchNorm):
            m.eval()


def run_eval(model, template, config, out, step, best, emit, device, guard, patience, dev_sets=DEV_SETS):
    model.eval()
    # Evaluate a CPU copy: eval batches have many distinct shapes, each of
    # which would otherwise leave a cached graph behind on MPS.
    t0 = time.time()
    cpu_model = copy.deepcopy(model).cpu().eval()
    # 30 s batches instead of 200 s: identical WER (padding is masked), but
    # ~3x faster for the 15x5 on CPU.
    results = evaluate_stores(cpu_model, template, dev_sets, torch.device('cpu'), batch_seconds=30.0)
    del cpu_model
    score = float(np.mean([r['wer'] for r in results.values()]))
    metrics = {'score': score, 'step': step, 'audio_hours': round(guard['audio_seconds'] / 3600, 2),
               'sets': {k: v['wer'] for k, v in results.items()}}
    emit(event='eval', step=step, score=round(score, 4), eval_s=round(time.time() - t0),
         audio_hours=metrics['audio_hours'], **{k: round(v['wer'], 4) for k, v in results.items()})
    # Every evaluated point is kept (~76 MB each for the 15x5).
    snapshot = out / 'evals' / f'step_{step:06d}'
    save_checkpoint(model, template, config, snapshot)
    (snapshot / 'metrics.json').write_text(json.dumps(metrics, indent=2))
    if best is None or score < best['score']:
        best = metrics
        guard['bad'] = 0
        save_checkpoint(model, template, config, out / 'best')
        (out / 'best' / 'metrics.json').write_text(json.dumps(best, indent=2))
        emit(event='new_best', step=step, score=round(score, 4))
        return best
    # Noise on the dev subsets is ~0.3% absolute: only a clear loss counts,
    # and a big one rolls back immediately.
    if score > best['score'] * 1.01:
        guard['bad'] += 1
    if (guard['bad'] >= patience or score > best['score'] * 1.05) and step != best['step']:
        _, state = load_checkpoint(out / 'best')
        model.load_nemo(state)
        guard['bad'] = 0
        guard['lr_scale'] *= 0.5
        emit(event='rollback', to_step=best['step'], score=round(best['score'], 4),
             lr_scale=guard['lr_scale'])
    return best


if __name__ == '__main__':
    main()
