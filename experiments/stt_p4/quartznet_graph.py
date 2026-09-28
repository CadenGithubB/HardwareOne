#!/usr/bin/env python3
"""Feature-only QuartzNet-5x5 graph, using the inspected tensor-only checkpoint.

Input [1,64,T,1], output [1,29,ceil(T/2),1]. T is the exact valid feature
length: no padded suffix, graph masks, frontend, softmax, or CTC decoder.
"""
from pathlib import Path
import hashlib
import torch
from torch import nn
import yaml

WEIGHTS_SHA256 = '51a92f946c8cccacb723f4291be1f3285d589e5ac84fa0f6714221dbbbf333cb'
CONFIG_SHA256 = '1e829dd42c31d4c55d2bcf844c8abbf664ba6f1b767a9e6c8e8d26672ae5ab58'
DEFAULT_CHECKPOINT = Path(__file__).resolve().parents[1] / 'speech_portable/private/stt-quartznet'


def load_checkpoint(directory=DEFAULT_CHECKPOINT):
    directory = Path(directory)
    for name, expected in [('model_weights.ckpt', WEIGHTS_SHA256), ('model_config.yaml', CONFIG_SHA256)]:
        if hashlib.sha256((directory/name).read_bytes()).hexdigest() != expected:
            raise ValueError('Checkpoint hash mismatch: ' + name)
    config = yaml.safe_load((directory/'model_config.yaml').read_text())
    state = torch.load(directory/'model_weights.ckpt', weights_only=True, map_location='cpu')
    return config, state


def make_conv(state, name, stride=1, dilation=1, batch_norm=None):
    weight = state[name+'.weight'].detach().clone().unsqueeze(-1)
    out_channels, per_group, kernel, _ = weight.shape
    groups = out_channels if per_group == 1 else 1
    in_channels = per_group * groups
    bias = state.get(name+'.bias')
    bias = bias.detach().clone() if bias is not None else None
    if batch_norm:
        # Inference BN is affine per output channel. Fold into the pointwise
        # convolution (or ordinary convolution), never the preceding depthwise.
        scale = state[batch_norm+'.weight'] / torch.sqrt(state[batch_norm+'.running_var'] + 1e-3)
        weight *= scale[:,None,None,None]
        if bias is None: bias = torch.zeros(out_channels, dtype=weight.dtype)
        bias = (bias-state[batch_norm+'.running_mean']) * scale + state[batch_norm+'.bias']
    layer = nn.Conv2d(in_channels, out_channels, (kernel,1), stride=(stride,1),
                      padding=(dilation*(kernel-1)//2,0), dilation=(dilation,1),
                      groups=groups, bias=bias is not None)
    layer.weight = nn.Parameter(weight, requires_grad=False)
    if bias is not None: layer.bias = nn.Parameter(bias, requires_grad=False)
    return layer


class Block(nn.Module):
    def __init__(self, state, index, config):
        super().__init__()
        prefix = f'encoder.encoder.{index}'
        separable = config.get('separable', False)
        width = 5 if separable else 4
        layers = []
        for repeat in range(config['repeat']):
            offset = repeat*width
            if separable:
                layers.append(make_conv(state, f'{prefix}.mconv.{offset}.conv',
                                        config['stride'][0], config['dilation'][0]))
                layers.append(make_conv(state, f'{prefix}.mconv.{offset+1}.conv',
                                        batch_norm=f'{prefix}.mconv.{offset+2}'))
            else:
                layers.append(make_conv(state, f'{prefix}.mconv.{offset}.conv',
                                        config['stride'][0], config['dilation'][0],
                                        batch_norm=f'{prefix}.mconv.{offset+1}'))
            if repeat+1 < config['repeat']: layers.append(nn.ReLU())
        self.main = nn.Sequential(*layers)
        self.residual = make_conv(state, prefix+'.res.0.0.conv', batch_norm=prefix+'.res.0.1') \
            if config.get('residual') else None
        self.relu = nn.ReLU()

    def forward(self, x):
        y = self.main(x)
        if self.residual is not None: y = y + self.residual(x)
        return self.relu(y)


class QuartzNet(nn.Module):
    def __init__(self, config, state):
        super().__init__()
        self.blocks = nn.ModuleList(Block(state, i, block) for i, block in enumerate(config['encoder']['params']['jasper']))
        self.head = make_conv(state, 'decoder.decoder_layers.0')

    def forward(self, x):
        for block in self.blocks: x = block(x)
        return self.head(x)

    def input_to_stage(self, x, stage):
        for block in self.blocks[:stage]: x = block(x)
        return x


def greedy(logits, vocabulary):
    ids = logits[0,:,:,0].argmax(dim=0).tolist()
    previous, text = None, []
    for token in ids:
        if token != previous and token < len(vocabulary): text.append(vocabulary[token])
        previous = token
    return ''.join(text).strip()


def word_errors(reference, hypothesis):
    a, b = reference.lower().split(), hypothesis.lower().split()
    row = list(range(len(b)+1))
    for i, left in enumerate(a, 1):
        next_row = [i]
        for j, right in enumerate(b, 1):
            next_row.append(min(next_row[-1]+1, row[j]+1, row[j-1]+(left != right)))
        row = next_row
    return row[-1]
