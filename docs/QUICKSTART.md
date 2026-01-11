# Quick Start Guide

This guide will help you get up and running with Hardware One

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
      > **NOTE Before continuing, you MUST ensure that the QT PY, BFF Module and EYESPI Adapter modules have _both_ a positive and negative cable connected to their proper pins. Failure to complete the circuit can result in blown components or battery.**
      16. Connect your Adafruit QT PY Pico to your computer via USB-C
      17. Continue to the software setup.
      

## Software Setup

1. Install the Arduino IDE (2.0 or later recommended)
   - https://support.arduino.cc/hc/en-us/articles/360019833020-Download-and-install-Arduino-IDE
2. Install the ESP32 board package
   - https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_dev_index.json
3. Configure the IDE:
   - Open the Serial Monitor and set it to 115200 baud
   - In the 'Tools' Menu at the top -
      - Set Board to "Adafruit QT PY ESP32"
      - Set Upload Speed to 115200
   - Install the libraries relevant to the hardware you're using.<br><br>
      - > NOTE: In the User Guide there is a table which features information about the necessary libraries.
 4. Depending on which 'configuration' you choose, the program you upload to the QT PY will differ. Pick the appropriate one:
   There are three ways this can work:
     > - 1: If you are planning to use exactly the same hardware outlined under the Hardware Requirements, you can download the pre-prepared program specific to the hardware configuration chosen. Options can be found here:
  Barebones
  Wired
  Wireless - https://github.com/CadenGithubB/HardwareOne/blob/main/Main/Main.ino

     > - This is recommended to get up and running quickly.

  Alternatively:

     > - 2: If you would like to use other hardware, or remove any code, its best for you to create your own program. The steps on how to do this are located in the UserGuide - https://github.com/CadenGithubB/HardwareOne/blob/main/UserGuide.md
      
5. Once you pick the program, download it.
6. Open the Main.ino file you downloaded or made in the Arduino IDE
7. Upload the code to your QT PY. Please be patient, this is close to the limit of the chip, so it can take a minute or two.
> Do **NOT** let the computer fall asleep during this time. If you do, you will need to restart the upload process.
8. Thats it for the software!
   - If you are using the 'Base Configuration' or 'HardwareOne (Wired)' the device will already be powered on, due to the USB C cable.
      - Sometimes resetting via the button on the QT PY is necessary if no console output is shown upon device boot / reboot
   - If you are using 'Hardware One (wireless)', disconnect the USB Device, then switch the Battery Module Power Switch into the 'On' position. (Or 'up' towards the top of the device.)


## First-Time Use

Once device is flashed and powered on:
   - You should see the boot sequence and error checking messages as it turns on, and then the main user environment once it boots up.
   - Type `help` to see all available commands

### Setting Up WiFi and Web Interface

**NEW**: Hardware One now includes a web-based interface for remote control and monitoring!

1. **Configure WiFi** (via Serial Console):
   ```
   setssid YourWiFiName
   setpass YourWiFiPassword
   wifi
   ```

2. **Start Web Server**:
   ```
   webstart
   ```
   - The device will display its IP address (e.g., `192.168.1.100`)
   - Open a web browser and navigate to that IP address
   - You'll see a live dashboard with all connected sensors!

3. **Web Interface Features**:
   - Real-time sensor data visualization
   - Start/stop sensors with buttons
   - Execute commands remotely
   - Live thermal camera display (if connected)
   - Multi-object distance detection

4. **Auto-start Web Server** (optional):
   ```
   webauto on
   ```
   - Web server will start automatically on boot

### Serial Console Commands
   - Type `help` to see all available commands
   - Use `scan` to detect connected sensors
   - Try sensor commands like `gyro`, `thermal`, `tof`, etc.
   - go nuts! 

> ## View the Readme: [Link](https://github.com/CadenGithubB/HardwareOne/blob/main/README.md)


> ## To take a deep dive on the capabilities of the project, check out the Userguide: [Link](https://github.com/CadenGithubB/HardwareOne/blob/main/USERGUIDE.md)

