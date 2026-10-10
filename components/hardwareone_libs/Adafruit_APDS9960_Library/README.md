# Adafruit_APDS9960 [![Build Status](https://github.com/adafruit/Adafruit_APDS9960/workflows/Arduino%20Library%20CI/badge.svg)](https://github.com/adafruit/Adafruit_APDS9960/actions)

<a href="https://www.adafruit.com/product/3595"><img src="assets/board.jpg?raw=true" width="500px"></a>

This is the Adafruit APDS9960 Proximity, Light, RGB, and Gesture sensor Library

Tested and works great with the Adafruit APDS9960 Board 
* http://www.adafruit.com/products/3595

This chip uses I2C to communicate, 2 pins are required to interface

Adafruit invests time and resources providing this open source code, please support Adafruit and open-source hardware by purchasing products from Adafruit!

Written by Dean Miller, Limor Fried for Adafruit Industries.  
BSD license, check license.txt for more information
All text above must be included in any redistribution

To install, use the Arduino Library Manager and search for "Adafruit APDS9960 Library" and install the library.

## Local gesture-reader fix

Hardware One makes `readGesture()` a polling operation: each call processes at
most one available FIFO batch and returns, without delays or waiting for more
input while holding the shared I2C bus. Direction signs persist between calls
and expire after 300 ms without directional activity. A completed gesture,
invalid FIFO level or failed transfer clears that state. Call regularly to
assemble gestures across batches; the public signature and direction codes
are unchanged.

FIFO levels count four-byte U/D/L/R datasets. The reader processes complete
datasets and rejects failed transfers. Direction markers retain only whether
a sign was seen, avoiding counter overflow during full FIFO batches.

The FIFO layout is described on pages 32–33 of the
[APDS9960 datasheet](https://cdn-learn.adafruit.com/assets/assets/000/045/848/original/Avago-APDS-9960-datasheet.pdf).
The `apds_gesture_driver` host suite executes the actual vendored methods with
controlled time and transport. Preserve this fix when updating the library.
