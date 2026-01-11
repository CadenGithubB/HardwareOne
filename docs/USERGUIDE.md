# This is the User Guide for this project. 
### It features:

-  [QT PY Features + Per Module Features](#qt-py-features--per-module-features)
-  [Required, Optional or Not-Required hardware](#required-optional-or-not-required-hardware)
-  [Available, Optional, or Not-Available software features](#available-optional-or-not-available-software-features)
-  [Hardware Setup](#hardware-setup)
-  [Software Setup](#software-setup)
-  [Required libraries](#required-libraries)
-  [Command List](#command-list)
-  [Building your own program](#how-to-build-your-own-hardware-one-program)
-  [Contact Me](https://cadenbeck.tech/home/contact/)
     
> NOTE: All information found in the QUICKSTART.md and README.md documents is spreadout through this, so it is a one stop shop for everything.<br>

## QT PY Features + Per Module Features

  The QT PY has several features on its own, but is best paired with additional modules. Its standalone capabilities are:

  - **WiFi**:
    - Manual AP Scan
    - Manual and Automatic WiFi connection
    - (wip) Credential Management (multiple SSIDs)
    - Persistent credential storage for one network
    - Auto-connect option on boot

  - **Bluetooth**:
    - Manual Bluetooth Scan
    - Manual and Automatic Bluetooth connection
    - (wip) Device Management (multiple devices)
    - Allows for Bluetooth keyboard use
    - Auto-connect option on boot for the primary bluetooth device* (primary aspect not implemented yet, but it just auto reconnects to the one device in storage.)

  - **I2C Devices**
    - Modify I2C Bus Speed
    - Reset I2C Bus
    - Scan I2C Bus
    - (wip) Device Management - enable or disable connected devices and their libraries.
    - Confirm health of select modules

  - **Command Interface**:
    - Serial command interface for device control
    - Web-based interface for remote control and monitoring
      - Browser-accessible dashboard 
      - Real-time sensor data visualization
      - Remote command execution
      - Live thermal camera display
      - Multi-object distance detection
    - Extensive help system
      - Simple to navigate menu
      - Provides in-depth descriptions and examples of commands, if requested. (hard coded, no logic)
    - View resource consumption (debugging and general use)
    - Refresh persistent console history with timestamp support
   
  - **Onboard Neopixel**:
    - Set color manually
    - blinkLED function - used to express device status
   
  - **Time**
    - NTP time synchronization
    - (Not started) Manual Timezone Adjustment

  - **Power Management**
  - Reboot Device
    - (everything past this is wip)
    - Sleep mode
    - set screen timeout
    - idk, more tho. 
   
  - **More to come...**
-----
### **Per Module Features**
  
  - Adafruit 5×5 NeoPixel Grid BFF Add-On for QT Py – [[ID: 5646]](https://www.adafruit.com/product/5646)
    - Addressable 5x5 grid of RGB Pixels 
         - <ins>Scrolling Text</ins><br><br>
           > Set custom message, and watch it scroll across the 5x5 Display.
           - Change Color
           - Change Speed
           - Change Text
        
  - Adafruit Buttons + Joystick – [[ID: 5743]](https://www.adafruit.com/product/5743)
    - Can read input
    - Nothing else, yet.
      
  - Adafruit LSM6DS3TR-C + LIS3MDL – Precision 9 DoF IMU – [[ID: 5543]](https://www.adafruit.com/product/5543)
    - Can read input
    - Nothing else, yet.
      
  - Adafruit APDS9960 Proximity, Light, RGB, and Gesture Sensor – [[ID: 3595]](https://www.adafruit.com/product/3595)
    - Nothing, yet.
      
  - Adafruit VL53L1X Time of Flight Distance Sensor –  [[ID: 3967]](https://www.adafruit.com/product/3967)
    - Can read input
    - Can log input
    - Nothing else, yet.
      
  - Adafruit VL53L4X Time of Flight Distance Sensor – [[ID: 5425]](https://www.adafruit.com/product/5425)
    - Can read input
    - Can log input
    - Nothing to take advantage of the dual sensing capabilities, yet.
      
  - Adafruit AMG8833 8x8px IR Thermal Camera – [[ID: 3538]](https://www.adafruit.com/product/3538)
    - Can read input
    - Can interpolate live data to provide a smoother "looking image" (This was done via numerical values on a command line, so it doesnt really look smoother. Once there is a graphical display it will look like a 64x64 image instead of an 8x8 image.
      
  - Adafruit I2S Amplifier BFF Add-On for QT Py – [ID: 5770]
    - Nothing, yet.

-----

## **Required, Optional or Not-Required hardware**

<ins>Key</ins>:  

❌ - Not Intended

🟡 - Optional, available in code

✅ - Intended

> NOTE: **If intended modules are not connected to the Systems they are inteded to be in, the commands involving those modules will gracefully fail.** Fortunately, its easy to get rid of extreneous commands/programs by cloning the relevant repo, and removing the code relevant to it. See this for more inf: Link (Put link here to link to the User Guide where the breakdown of the code for each module is explained. "each module has code in the following areas: 'startup checking', 'error checking', 'connectivity', 'function', 'variables', etc")

🛠️ - You choose! If you program capabilities for a new module, please let me know somehow so I can fold it into the mix of available options.

| ⬇️ Hardware / Configuration Names ➡️  | Barebones QT PY | Hardware One (Wired) | Hardware One (Wireless) | DIY System
| ------------- | :-----------: | :-----------: | :-----------: | :-----------: |
| Adafruit QT PY - [[ID: 5395]](https://www.adafruit.com/product/5395) | ✅ | ✅ | ✅ | 🛠️
| Mini Breadboard - [[Link]](https://www.amazon.com/dp/B01N9MIH1T?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_3) | ❌ | ✅ | ✅ | 🛠️
| EYESPI Display BFF - [[ID: 5572]](https://www.adafruit.com/product/5772) | ❌ | ✅ | ✅ | 🛠️
| EYESPI Display - [[ID: 4311]](https://www.adafruit.com/product/4311) | ❌ | ✅ | ✅ | 🛠️
| Buttons + Joystick - [[ID: 5743]](https://www.adafruit.com/product/5743) | ❌ | ✅ | ✅ | 🛠️
| LSM6DS3TR-C + LIS3MDL Gyroscope - 6/9DoF - [[ID: 5543]](https://www.adafruit.com/product/5543) | ❌ | ✅ | ✅ | 🛠️
| RGB + Gesture + Light Measuring Sensor - [[ID: 3595]](https://www.adafruit.com/product/3595) | ❌ | ✅ | ✅ | 🛠️
| VL53L1X / VL53L4X Distance Sensor - [[ID: 3967]](https://www.adafruit.com/product/3967) / [[ID: 5425]](https://www.adafruit.com/product/5425) | ❌ | ✅ | ✅ | 🛠️
| Battery - 14500 Li-ion w/ a JST 2.0 connector | ❌ | ❌ | ✅ | 🛠️
| QT PY Battery BFF - [[ID: 5646]](https://www.adafruit.com/product/5397) | ❌ | ❌ | ✅ | 🛠️
| 5×5 NeoPixel QT PY BFF - [[ID: 5646]](https://www.adafruit.com/product/5646) | ❌ | 🟡 | 🟡 | 🛠️
| 8x8px IR Thermal Camera - [[ID: 3538]](https://www.adafruit.com/product/3538) | ❌ | 🟡 | 🟡 | 🛠️
| Stemma QT Hub (Port Duplicator / 'Dumb' Hub) - [[ID: 5625]](https://www.adafruit.com/product/5625) | ❌ | 🟡 | 🟡 | 🛠️
| Intended for DIY | ❌ | ❌ | ❌ | ✅

-----

## **Available, Optional, or Not-Available software features**
  
<ins>Key</ins>:  

❌ - Not Available

🟡 - Optional

✅ - Available

⌨️ - Custom Configuration - I will provide the basic building blocks so someone can edit it and keep, remove, or add new features. Please make your own versions and share them with me!

| ⬇️ Features / Configuration Names ➡️  | Barebones QT PY | Hardware One (Wired) | Hardware One (Wireless) | DIY System
| ------------- | :-----------: | :-----------: | :-----------: | :-----------: |
| Base QT PY Function (WiFi, Bluetooth, Time Keeping, i2c scanning) | ✅ | ✅ | ✅ | ✅/⌨️
| Console Output (Via USB-C) | ✅ | ✅ | ✅ | ✅
| Web Interface (Browser-based Control) | ✅ | ✅ | ✅ | ✅/⌨️
| Dedicated Input Device (Joystick + Buttons) | ❌ | ✅ | ✅ | ✅/⌨️
| Display Output (Via an onboard screen) | ❌ | ✅ | ✅ | ✅/⌨️
| Distance Sensor (Multi-object Detection) | ❌ | ✅ | ✅ | ✅/⌨️
| RGB Capture + Gesture and Light Sensing | ❌ | ✅ | ✅ | ✅/⌨️
| 6/9 DOF Gyroscope | ❌ | ✅ | ✅ | ✅/⌨️
| 5×5 RGB NeoPixels | ❌ | 🟡 | 🟡 | 🟡/⌨️
| Thermal Scanning | ❌ | 🟡 | 🟡 | 🟡/⌨️
| Battery Meter | ❌ | ❌ | ✅ | ✅/⌨️
| PSRAM Optional (Works with all ESP32 configs) | ✅ | ✅ | ✅ | ✅/⌨️
| DIY Required | ❌ | ❌ | ❌ | ✅

-----

## Hardware Setup

   - **For the 'Base Configuration', start here:**
      - Connect your Adafruit QT PY Pico to your computer via USB-C.
      - Continue to Software Setup.

   - **For 'Hardware One (Wired)', start here:**
      > NOTE: This assumes that you have already soldered pins to the QT PY, EYESPI Adatper and Battery BFF modules. If you dont, please do that to continue.
      1. (**_This is optional_**) Select 0-4 STEMMA QT modules and connect them to the STEMMA QT connector of the QT PY:
      2. USB C port first, insert the QT PY into the case. Push the QT PY flat against the bottom of the case.
      3. Slot any connected Stemma QT sensor modules into the Case towards the top. Leave the Gamepad off to the side. Manage the cables.
      4. Connect the EYESPI Display Cable to the EYESPI Adapter.
      5. Slide the EYESPI display cable through the slot between the space for the battery and EYESPI Adapter, eventually placing the EYESPI Adapter flat against the bottom of the case.<br><br>
      > NOTE: If you are going the DIY route, only continue to the next step once you have a proper diagram of the setup you want. This is where you can begin to damage components, so continue with caution.
      6. Connect the wires like so: (put link here to a nice looking wire diagram. Need to find software for that.)<br><br>
      7. With the device wired up, insert the Midcase (new word?) into the device to cover and protect the wires below. Make sure the Gamepad and EYESPI Display Cable are able to be accessed from above the assembled case.
      8. Take the EYESPI Display Cable and lay it flat over the Midcase, so its available end lays in the middle, near the top. 
      9. Place the Gamepad into its spot in the Case, over the EYESPI Display Cable, Midcase, Breadboard and EYESPI Display Module. (In that order, from the top down.)<br><br>
      > The Case is now assembled! Now you just need to put the screen into the Top of the case, and connect the screen.
      10. Take the Top Case, slide the screen into it, and insert the shim to keep it in place.
      11. Attach the EYESPI Display Cable into the connector on the EYESPI Display.
      12. Put the Top Case and the Bottom Case together, and you are finished assembling the device.
      > **NOTE Before continuing, you MUST ensure that the QT PY, BFF Module and EYESPI Adapter modules have _both_ a positive and negative cable connected to their proper pins. Failure to complete the circuit can result in blown components or battery... complications.**
      13.  Connect your Adafruit QT PY to your computer via USB-C
      14.  Continue to the software setup.
      
   - **For 'Hardware One (Wireless)', start here:**
      1. Place the battery in the case, taking care to route the cables into the cavity below where the QT PY will sit.
      2. Follow all Hardware Setup steps 1 through 5 for the 'Hardware One (Wired)' setup above. Continue from here once you finish step 5.
      6. Connect the wires like so: (put link here to a nice looking wire diagram. Need to find software for that.)<br><br>
      > Its best to lift the battery and route the EYESPI Display Cable under the battery. If its not, the cable can crease once everything is closed up. ( Its not the end of the world, but its good to avoid. )
      7. With the device wired up, insert the Midcase (new word?) into the device to cover and protect the wires below. Make sure the Gamepad and EYESPI Display Cable are able to be accessed from above the assembled case.
      8. Take the EYESPI Display Cable and lay it flat over the Midcase, so its available end lays in the middle, near the top.
      9. Place the Gamepad into its spot in the Case, over the EYESPI Display Cable, Midcase, Breadboard and EYESPI Display Module. (In that order, from the top down.)
      10. Put the power switch of the Battery BFF Module in the 'Off' position.
      11. Connect the battery connector to the Battery BFF, then place the Battery BFF Module into the case. Press it flat against the bottom.<br><br>
      12. Put in the case power switch, and attach the outer piece. (wip, glue needed)<br><br>
      > The Bottom Case is now assembled! Now you just need to put the screen into the Top Case, and connect the screen.
      13. Take the Top Case, slide the screen into it, and insert the shim to keep it in place.
      14. Attach the EYESPI Display Cable into the connector on the EYESPI Display.
      15. Put the Top Case and the Bottom Case together, and you are finished assembling the device.
      > **NOTE Before continuing, you MUST ensure that the QT PY, BFF Module and EYESPI Adapter modules have _both_ a positive and negative cable connected to their proper pins. Failure to complete the circuit can result in blown components or battery... complications.**
      16. Connect your Adafruit QT PY Pico to your computer via USB-C
      17. Continue to the software setup.
      

## Software Setup

- Install the Arduino IDE (2.0 or later recommended)
   - https://support.arduino.cc/hc/en-us/articles/360019833020-Download-and-install-Arduino-IDE
- Install the ESP32 board package
   - https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_dev_index.json
- Configure the IDE:
   - Open the Serial Monitor and set it to 115200 baud
   - In the 'Tools' Menu at the top -
      - Set Board to "Adafruit QT PY ESP32"
      - Set Upload Speed to 115200
   - Install the libraries relevant to the hardware you're using.<br><br>
      - > NOTE: In the User Guide there is a table which features information about the necessary libraries.
- Depending on which 'configuration' you choose, the program you upload to the QT PY will differ. Pick the appropriate one:
   There are three ways this can work:
     > - 1: If you are planning to use exactly the same hardware outlined under the Hardware Requirements, you can download the pre-prepared program specific to the hardware configuration chosen. Options can be found here:
  Barebones
  Wired
  Wireless - https://github.com/CadenGithubB/HardwareOne/blob/main/Main/Main.ino

     > - This is recommended to get up and running quickly.

  Alternatively:

     > - 2: If you would like to use other hardware, or remove any code, its best for you to create your own program. The steps on how to do this are located in the UserGuide - https://github.com/CadenGithubB/HardwareOne/blob/main/UserGuide.md
      
- Once you pick the program, download it.
- Open the Main.ino file you downloaded or made in the Arduino IDE.<br><br>
     > Before compiling the program and uploading it to the QT PY, you need to install the required libraries. See [Required libraries](#required-libraries) to find what libraries you need.
- Upload the code to your QT PY. Please be patient, this is close to the limit of the chip, so it can take a minute or two.<br><br>
> Do **NOT** let the computer fall asleep during this time. If you do, you will need to restart the upload process.
- Thats it for the software!
   - If you are using the 'Base Configuration' or 'HardwareOne (Wired)' the device will already be powered on, due to the USB C cable.
      - Sometimes resetting via the button on the QT PY is necessary if no console output is shown upon device boot / reboot
   - If you are using 'Hardware One (wireless)', disconnect the USB Device, then switch the Battery Module Power Switch into the 'On' position. (Or 'up' towards the top of the device.)


## Required Libraries

 To utilize the modules and the commands that work with them, you will need to do two things:
 > - 1) install the libraries for the specific modules you are using in the Arduino Library Manager.<br><br>
        - If you're unsure of the name of the library to search for it, it can often be found on the adafruit webpage. If not, a quick google will show it._<br><br>
  > - 2) Ensure to
>        > #include libraryname
>        in your program. Examples below. <br>
  
For the QT PY, the following libraries are used:

- For the 'Base Configuration', utilize these libraries:
     - WiFi
       ```
         #include <WiFi.h>
       ```
  Library: https://github.com/espressif/arduino-esp32/tree/master/libraries/WiFi
  
     - SPIFFS / Storage
         ```
         #include <SPIFFS.h>
         #include <FS.h>
         ```
  Library: https://github.com/espressif/arduino-esp32/tree/master/libraries/SPIFFS
  
     - Wire
         ```
         #include <Wire.h>
         ```
  
  Library: https://docs.arduino.cc/language-reference/en/functions/communication/wire/

     - Time / NTP
         ```
         #include <time.h>
         #include <esp_sntp.h>
         ```
  Library: https://docs.arduino.cc/libraries/time/


For additional I2C modules, the following libraries are used
 
  > The following I2C Devices have been tested and confirmed to work:
  - Adafruit 5×5 NeoPixel Grid BFF Add-On for QT Py – [ID: 5646]
       - Addressable RGB Pixels
           ```
           #include <Adafruit_NeoPixel.h>
           ```
    - Library: https://github.com/adafruit/Adafruit_NeoPixel
      
  - Adafruit Buttons + Joystick – [ID: 5743]
      - Non-Keyboard / Controller Input 
           ```
           #include "Adafruit_seesaw.h"
           ```
    - Library: https://github.com/adafruit/Adafruit_Seesaw
      
  - Adafruit LSM6DS3TR-C + LIS3MDL – Precision 9 DoF IMU – [ID: 5543]
      - Gyroscope Sensor
           ```
           #include <Adafruit_LSM6DS33.h>
           #include <Adafruit_LSM6DS3TRC.h>
           ```
    - Library: https://github.com/adafruit/Adafruit_LSM6DS + https://github.com/adafruit/Adafruit_LIS3MDL
      
  - Adafruit APDS9960 Proximity, Light, RGB, and Gesture Sensor – [ID: 3595]
       - Various Sensors
           ```
           #include "Adafruit_APDS9960.h"
           ``` 
    - Library: https://github.com/adafruit/Adafruit_APDS9960
      
  - Adafruit VL53L1X Time of Flight Distance Sensor – [ID: 3967]
       - Distance Sensor
           ```
           #include "Adafruit_VL53L1X.h"
           ``` 
    - Library: https://github.com/adafruit/Adafruit_VL53L1X
      
  - Adafruit VL53L4X Time of Flight Distance Sensor – [ID: 5425]
       - Improved Distance Sensor
           ```
          #include <Arduino.h>
          #include <vl53l4cx_class.h>
          #include <string.h>
          #include <stdlib.h>
          #include <stdio.h>
          #include <stdint.h>
          #include <assert.h>
          #include <stdlib.h>
            ```
    - Library: https://github.com/stm32duino/VL53L4CX
      
  - Adafruit AMG8833 IR Thermal Camera – [ID: 3538]
       - Thermal Sensor
           ```
           #include <Adafruit_AMG88xx.h>
           ```
    - Library: https://github.com/adafruit/Adafruit_AMG88xx
      
  - Adafruit I2S Amplifier BFF Add-On for QT Py – [ID: 5770]
    - Library: wip
    - Support for scanning and detecting other I2C devices

-----

## Command list

Commands for the QT PY:

  - **Wifi Related Commands/Functions**
    - 'wifion' – Toggle Wifi On
    - 'wifioff' – Toggle Wifi Off
    - 'autoconnect' – Toggle Wifi AutoConnect
    - 'wificonnect' – Connect to Wifi Network
    - 'wifidisconnect' – Disconnect from Wifi Network
    - 'setssid <ssid>' – Save SSID
    - 'setpass <pass>' – Save Password
    - 'scanap' – Scan for Nearby Access Points
    - 'clearwifi' – Clear Saved Wifi Credentials
    
  - **Web Interface Commands**
    - 'webstart' – Start web server
    - 'webstop' – Stop web server
    - 'webstatus' – Show web server status and IP address
    - 'webauto <on/off>' – Enable/disable web server auto-start on boot
    - Access web interface by navigating to the device's IP address in a browser
      
  - **Bluetooth Related Commands/Functions**
    - 'bton' – Toggle Bluetooth On
    - 'btoff' – Toggle Bluetooth Off
    - 'btautoconnect' – Toggle Bluetooth AutoConnect
    - 'btconnect' – Connect to Bluetooth Devices
    - 'btdisconnect' – Disconnect from Wifi Network
    - 'btscan' – Scan for Nearby Bluetooth Devices
    - 'btclear' – Clear Saved Bluetooth Devices

  - **I2C Related Commands/Functions**
    - 'scan' – Scan I2C Bus and detect connected sensors
    - 'i2cspeed' – Modify I2C Bus Speed
    - 'i2creset' – Reset I2C Bus
    - 'i2ccheck' – Confirm health of select modules

  - **Time Related Commands/Functions**
    - 'synctime' – Sync Time from an NTP server
    - 'time' – Display Time
   
  - **Neopixel Related Commands/Functions**
    - 'ledcolor <color> ' – Set the Neopixel to a color. Can enter multiple colors. 
    - 'clearledcolor' – Turns off the LED.
    
  - **Console History and System Commands**
    - 'sysinfo' – Display system information (memory, uptime, sensors)
    - 'ping' – Test system responsiveness
    - 'historysize <number>' – Set console history buffer size (1-50)
    - 'historyclear' – Clear console history
    - 'help' – Show all available commands
    - **Refresh persistent console history with real-time timestamps**

Commands for the optional hardware modules:

  - **Gyro + Accelerometer**
    - ‘gyro’ – Toggles Gyro output
    - ‘accel’ – Toggles Accel. output
      
  - **Time of Flight Sensor (VL53L4CX)**
    - 'tof' – Start/stop single distance measurement
    - 'tofstart' – Start continuous ToF distance polling
    - 'tofstop' – Stop continuous ToF distance polling
    - 'tofobjects' – Show all detected objects (up to 4 simultaneous)
    - **NEW: Multi-object detection with 6-meter range**
      
  - **RGB Gesture Sensor (APDS-9960)**
    - 'color' – Start/stop color sensor readings (R,G,B,Clear)
    - 'proximity' – Start/stop proximity detection
    - 'gesture' – Start/stop gesture recognition (up/down/left/right)
    - **NEW: Separate controls for each sensor mode**
      
  - **User Controls (Gamepad)**
    - 'gamepad' – Start/stop gamepad input monitoring
    - **NEW: Fully functional in web interface with visual feedback**
      
  - **Thermal Camera Sensor (AMG8833)**
    - 'thermal' – Start/stop thermal sensor readings
    - **NEW: Live 8x8 thermal grid visualization in web interface**
    - **NEW: HSL color mapping for improved temperature visualization**
    - **NEW: Min/max/average temperature statistics**

-----

## How to build your own Hardware One Program

Below is a breakdown of the basic parts of the program, and what pieces of code are required for basic function, or required for particular modules / functions.

Install all required libraries:

   - For the QT PY, utilize these libraries:
      - WiFi
         ```
         #include <WiFi.h>
         ```
      - SPIFFS / Storage
         ```
         #include <SPIFFS.h>
         #include <FS.h>
         ```
      - Wire
         ```
         #include <Wire.h>
         ```
      - Time / NTP
         ```
         #include <time.h>
         #include <esp_sntp.h>
         ```
         
- For the other modules, these libraries are only required when the hardware is being utilized.

      
  > The following I2C Devices have been tested and confirmed to work:
  
   - Addressable RGB Pixels – [ID: 5646]
     ```
     #include <Adafruit_NeoPixel.h>
     ```
     - Library: https://github.com/adafruit/Adafruit_NeoPixel

      
   - Non-Keyboard / Controller Input  – [ID: 5743]
     ```
     #include "Adafruit_seesaw.h"
     ```
     - Library: https://github.com/adafruit/Adafruit_Seesaw
      
  - Gyroscope Sensor – [ID: 5543]
    ```
    #include <Adafruit_LSM6DS33.h>
    #include <Adafruit_LSM6DS3TRC.h>
    ```
    - Library: https://github.com/adafruit/Adafruit_LSM6DS + https://github.com/adafruit/Adafruit_LIS3MDL
      
  - Proximity, Light, RGB, and Gesture Sensors – [ID: 3595]
    ```
    #include "Adafruit_APDS9960.h"
    ``` 
    - Library: https://github.com/adafruit/Adafruit_APDS9960
      
  - Distance Sensor – [ID: 3967]
    ```
    #include "Adafruit_VL53L1X.h"
    ``` 
    - Library: https://github.com/adafruit/Adafruit_VL53L1X
      
  - Improved Distance Sensor – [ID: 5425]
    ```
    #include <Arduino.h>
    #include <vl53l4cx_class.h>
    #include <string.h>
    #include <stdlib.h>
    #include <stdio.h>
    #include <stdint.h>
    #include <assert.h>
    #include <stdlib.h>
    ```
    - Library: https://github.com/stm32duino/VL53L4CX
      
  - 8x8 Thermal Sensor – [ID: 3538]
    ```
    #include <Adafruit_AMG88xx.h>
    ```
    - Library: https://github.com/adafruit/Adafruit_AMG88xx
      
  - Adafruit I2S Amplifier BFF Add-On for QT Py – [ID: 5770]
    - Library: wip
    - Support for scanning and detecting other I2C devices

-----
      
> ## To get started, check out the Quickstart: [Link](https://github.com/CadenGithubB/HardwareOne/blob/main/QUICKSTART.md)



> ## Check out the readme: [Link](https://github.com/CadenGithubB/HardwareOne/blob/main/README.md)


## License

MIT License
