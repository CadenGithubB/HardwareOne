# This project is called Hardware One. 
> It is the combination of Adafruit hardware and libraries, with the software found in this github. Once assembled and programmed, it provides the user with a small device that can fit nearly any usecase. The device can be adjusted by adding modules such as environmental sensors, human input devices, screens or audio output, and more to accomplish whatever is required.

## There are two premade configurations to choose from, in addition to the DIY option:

  ###   1) The '<ins>Barebones QT PY</ins>' - 
   - This is the most basic version which utilizes only the QT PY and the serial console of an Arduino IDE to provide an interface for the device to communicate with the user.<br><br>
   - **This version serves as a showcase as to what the QT PY can accomplish on its own.**



  ###   2) '<ins>Hardware One</ins>' -
  - This is the 'main' version of the project that utilizes what I will call the 'standard set of hardware and software'. Everything will be made with this setup in mind, and paired down from there for the barebones version.<br><br>
  - **This version enables the use of Hardware One with or without a battery, to allow for usage anywhere.**

-----

For those interested in exploring the full capabilities of the QT PY / Hardware One, the DIY System offers a hands on experience.

  ###   The '<ins>DIY System</ins>'  - 
  - This is an option for tinkerers who want to customize Hardware One to their specific needs. I encourage people who do this to clone/fork the main 'Hardware One' code body. 
  - **This is a custom approach to using the QT PY and this project.**

-----

## Software Features Table
  
<ins>Software Features Key</ins>:  

❌ - Not available 

🟡 - Optional

✅ - Available

⌨️ - Custom Configuration - I will provide the basic building blocks so someone can edit it and keep, remove, or add new features. Please make your own versions and share them with me!

| ⬇️ Features / Configuration Names ➡️  | Barebones QT PY | Hardware One (Wired) | Hardware One (Wireless) | DIY System
| ------------- | :-----------: | :-----------: | :-----------: | :-----------: |
| Base QT PY Function (WiFi, Bluetooth, Time Keeping, i2c scanning) | ✅ | ✅ | ✅ | ✅/⌨️
| Console Output (Via USB-C) | ✅ | ✅ | ✅ | ✅
| Web Interface (Browser-based Control) | ✅ | ✅ | ✅ | ✅/⌨️
| ESP-NOW (Peer to peer device communication) | ✅ | ✅ | ✅ | ✅/⌨️
| WIP Servo Control via PCA9685 WIP | 🔨 | 🔨 | 🔨 | 🔨
| Dedicated Input Device (Joystick + Buttons) | ❌ | ✅ | ✅ | ✅/⌨️
| Display Output (Via an onboard screen) | ❌ | ✅ | ✅ | ✅/⌨️
| Distance Sensor (Multi-object Detection) | ❌ | ✅ | ✅ | ✅/⌨️
| RGB Capture + Gesture and Light Sensing | ❌ | ✅ | ✅ | ✅/⌨️
| 6/9 DOF Gyroscope | ❌ | ✅ | ✅ | ✅/⌨️
| 5×5 RGB NeoPixels | ❌ | 🟡 | 🟡 | 🟡/⌨️
| Thermal Scanning | ❌ | 🟡 | 🟡 | 🟡/⌨️
| WIP Haptic Motor WIP | 🔨 | 🔨 | 🔨 | 🔨
| Battery Meter | ❌ | ❌ | ✅ | ✅/⌨️
| PSRAM Optional (Works with all ESP32 configs) | ✅ | ✅ | ✅ | ✅/⌨️
| DIY Required | ❌ | ❌ | ❌ | ✅

-----

## Hardware Requirement Table

<ins>Hardware Requirements Key</ins>:  

❌ - Not Intended

🟡 - Optional, available in code

✅ - Intended

> NOTE: **If intended modules are not connected to the respective Systems, the commands involving those modules will gracefully fail.** Fortunately, its easy to get rid of extreneous commands/programs by cloning the relevant repo, and removing the code relevant to it. See this for more inf: Link (Put link here to link to the User Guide where the breakdown of the code for each module is explained. "each module has code in the following areas: 'startup checking', 'error checking', 'connectivity', 'function', 'variables', etc")

🛠️ - You choose! If you program capabilities for a new module, please let me know somehow so I can fold it into the mix of available options.

| ⬇️ Hardware / Configuration Names ➡️  | Barebones QT PY | Hardware One (Wired) | Hardware One (Wireless) | DIY System
| ------------- | :-----------: | :-----------: | :-----------: | :-----------: |
| Adafruit QT PY | ✅ | ✅ | ✅ | 🛠️
| Mini Breadboard | ❌ | ✅ | ✅ | 🛠️
| EYESPI Display BFF | ❌ | ✅ | ✅ | 🛠️
| EYESPI Display | ❌ | ✅ | ✅ | 🛠️
| Buttons + Joystick | ❌ | ✅ | ✅ | 🛠️
| Gyroscope - 6/9DoF | ❌ | ✅ | ✅ | 🛠️
| RGB + Gesture + Light Measuring device | ❌ | ✅ | ✅ | 🛠️
| Distance Sensor | ❌ | ✅ | ✅ | 🛠️
| Battery | ❌ | ❌ | ✅ | 🛠️
| QT PY Battery BFF | ❌ | ❌ | ✅ | 🛠️
| 5×5 NeoPixel QT PY BFF | ❌ | 🟡 | 🟡 | 🛠️
| 32x24 Thermal Camera | ❌ | 🟡 | 🟡 | 🛠️
| Haptic motor driver | ❌ | 🟡 | 🟡 | 🛠️
| Stemma QT Hub (Port Duplicator / 'Dumb' Hub)| ❌ | 🟡 | 🟡 | 🛠️
| Ability to add new hardware | ❌ | ❌ | ❌ | ✅

> ## To get started, check out the Quickstart: [Link](https://github.com/CadenGithubB/HardwareOne/blob/main/QUICKSTART.md)


> ## To take a deep dive on the capabilities of the project, check out the Userguide: [Link](https://github.com/CadenGithubB/HardwareOne/blob/main/USERGUIDE.md)
