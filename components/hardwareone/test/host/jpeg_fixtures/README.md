# Original JPEG test fixtures

These synthetic patterns were generated for HardwareOne by `generate.py` using
Pillow 12.3.0. No external pictures or private data are included. The binary files
are committed so normal tests do not require Pillow or regenerate encoder output.

The corpus covers 4:4:4 / 4:2:2 / 4:2:0 chroma sampling, odd 17x19 geometry,
MCU-padded 24x24 geometry, 320x240 timing inputs, grayscale, progressive JPEG and
solid red/blue images that detect accidental RGB/BGR reversal. A progressive
fixture checks graceful unsupported behavior; it does not assert that the legacy
software decoder supports progressive JPEG.

The additional 48x48 4:4:4 pattern and solid red/blue fixtures exceed the P4
driver's horizontal DMA block width, allowing physical accelerated RGB checks.

The solid 640x480 and 1280x720 files exercise Edge Impulse's VGA RGB-buffer
boundary and rejection of larger camera images. They contain generated color
only, with no camera photographs.
