# Hardware One — Quick Explanation

**Hardware One is a modular ESP32 firmware that works like a distributed operating system for cheap microcontrollers.**

You compile and flash each ESP32 chip to fit a specific job — a smart-home sensor, a Smart Glasses companion gadget made to live in your pocket or backpack, a headless mesh node, a camera node that captures photos and beams them across the mesh, and many more. No matter what feature set a device is built for, every one of them speaks the same custom ESP-NOW protocol, letting them form a private, router-free mesh. And because each device brings its own capabilities to that mesh, they can pool their data and work together — turning a scattered collection of chips into one system you can monitor from a single dashboard.

---

## Features

### Control — one command system, reached many ways

Every interface runs on a single CLI. A command issued through any endpoint flows
through the same pipeline with the same auth checks, and the result comes back out
the same endpoint — so each of these is a full two-way window into the device:

- **Serial** — terminal over USB
- **Web** — browser dashboard over WiFi
- **On-device** — OLED screen + gamepad
- **Phone** — app over Bluetooth (BLE)
- **G2 smart glasses** — gestures in, a lens-driven UI out, over BLE
- **Another node** — commands in, output streamed back, over the ESP-NOW mesh
- **Voice** — speak commands (ESP-SR wake word); results show on whatever screen you're at

### Networking

- **Custom encrypted ESP-NOW mesh** — private, router-free device-to-device protocol
- **WiFi** — connect, auto-reconnect, AP scan

### Sensors & capture

- **Automated I2C bus management** — hardware auto-detection, health monitoring and recovery across a broad sensor suite
- **Camera & microphone capture**

### Automation

- **Scheduling engine** — automations fire on specific times (daily / weekly / biweekly / monthly / yearly, with day-of-week targeting), repeating intervals, at boot, or on manual trigger — up to 4 triggers per automation
- **Conditional command builder** — `IF / THEN / ELSE` (and `ELSE IF`) logic that gates commands on live context: **sensor readings** (temperature, distance, light, motion), **time-of-day** (morning / afternoon / evening / night), and a device's own **mesh metadata** (room, zone, tags). Comparisons with `>`, `<`, `=`, `!=`, `>=`, `<=`, and `CONTAINS`. Any CLI command can be the action — so an automation can do anything you can type:

  > `IF MOTION = DETECTED THEN device set fan on ELSE device set fan off`

  Conditions are syntax-checked before they're saved, each automation is owned by the user who made it, and the whole evaluator runs zero-heap so it's safe on the main loop.

### System

- **Filesystem & storage** — LittleFS + optional SD card, a web file manager, and CSV data logging
- **Multi-user authentication** — accounts, roles & permissions, sessions, hardened password hashing, IP banning
- **Cryptography** — Ed25519 / X25519 + ChaCha20-Poly1305 secure the ESP-NOW mesh and a BLE secure channel; TLS/HTTPS available for the web UI
- **Memory & task diagnostics** — live heap/PSRAM monitoring and per-task stack watermarks (`memsample` / `memreport`)
- **Debug system** — per-subsystem debug flags routed to serial, web, file, or OLED simultaneously
- **Power management** — CPU frequency scaling, sleep modes, display brightness, and battery monitoring
- **First-time setup wizard** — guided onboarding on web + OLED
- **Bonded microcontrollers** — pair two boards so they share command registries, letting one device borrow another's features over ESP-NOW

### Applications

- **Offline GPS maps** — map viewer with waypoints and track logging
- **On-device LLM** — runs a built-in help agent that teaches you the system
- **Photo & video** — capture stills, record video, browse a gallery, and play recordings back in the browser (in-browser AVI/MJPEG player)
- **Thermal imaging** — live heatmap from the MLX90640 thermal camera
- **FM radio** — tune, seek, and listen via the RDA5807 receiver
- **Servo / PWM control** — drive up to 16 channels for robotics, animatronics, or motors (PCA9685)
- **Edge Impulse ML** — run custom machine-learning models for on-device detection / classification
- **Home Assistant integration** — publish sensors and subscribe to topics over MQTT (off by default)
- **Browser games** — built-in web games (A Dark Room, tilt maze)
