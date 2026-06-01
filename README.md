ESP32 Passive Network Monitor
=============================
Pre-Release
------------------
This is a working project, but it is not refined. Some assembly required. For example, the 3d printed case doesn't fit the buzzer and doesn't have a shield to protect the front of the case. Feel free to update and optimize.

Design
------------------

Multi-protocol network monitoring tool built on the ESP-IDF. This firmware passively scans for 802.11 management frames (Beacons, Probe Requests, Probe Responses) and Bluetooth Low Energy (BLE) advertisements. It cross-references captured MAC addresses, OUIs, and SSIDs against embedded watchlists, triggers GPIO alerts upon detection, and logs matched targets to non-volatile SPIFFS memory.

Supported Hardware
------------------

This project leverages static DRAM allocation and IRAM-pinned interrupt callbacks to handle high-density RF environments without dropping packets. It is explicitly designed to compile across multiple ESP32 architectures:

*   **ESP32-C5:** Supports dual-band channel hopping (2.4 GHz & 5 GHz).
    *   Hardware pins:
        *   Buzzer (GPIO 9)
        *   LED (GPIO 27)
*   **ESP32-C6:** High-efficiency 2.4 GHz monitoring.
    *   Hardware pins:
        *   Buzzer (GPIO 20)
        *   LED (GPIO 15)
*   **ESP32-S3:** 2.4 GHz monitoring.
    *   Hardware pins:
        *   Buzzer (GPIO 8)
        *   LED (GPIO 21)

Compile for your specific hardware. The C5 build automatically includes 5 GHz channel sweeping logic.

``` bash
idf.py set-target esp32x#
```

Watchlists and Configuration
----------------------------

The system uses hardcoded static arrays to identify targets of interest. This is highly effective for auditing specific infrastructure, verifying authorized broadcasts, or tracking fleet vehicles equipped with distinct mobile routers.

These arrays have basic targets and ignore lists. You will need to update these for your specific area. Review alerts, determine false positives, and update accordingly. Update these arrays in main.c to fit your operational requirements:

*   **target\_ouis**: Matches the first 3 bytes of a MAC address to identify specific hardware vendors.
*   **target\_ssids**: Substring matching for Wi-Fi network names.
*   **target\_ble\_names**: Substring matching for BLE advertisement names.
*   **ignore\_ssids**: Blacklist arrays to filter out false positives (e.g., expected ambient networks).

SPIFFS Target Logging
---------------------

When a device matches your watchlist, the alert task activates the GPIO buzzer/LED and permanently logs the target to the onboard flash memory (/spiffs/targets.txt). This ensures data survives reboots or power loss during field operations.

The system reads this log on boot and outputs it to the console. The log file is formatted as follows:

``` bash
--- SAVED TARGETS ---
MAC Address       | Total | P-Req | P-Rsp | Beacons | RSSI | SSID
xx:xx:xx:xx:xx:xx | 3     | 0     | 0     | 3       | -48  | Network | [YYYY-MM-DD 20:10:00] | 38.89555, -77.02519
xx:xx:xx:xx:xx:xx | 3     | 0     | 0     | 3       | -59  | Network Guest | [YYYY-MM-DD 20:10:00] | 38.89555, -77.02519
```

GPS Tracking
---------------------

When a Seeed Studios L76K GPS antenna is installed to the scanner, it will automatically detect GPS and append date and GPS coordinates to logged detections. If there is no GPS, the program will continue without date or location tracking.

Live Terminal Output
--------------------

The console refreshes every 5 seconds, providing a real-time, sorted view of the surrounding RF environment. Devices that match triggering conditions appear at the top. Devices are cleared from the live view after 30 seconds of inactivity to maintain a clean, active operational picture.

Build Instructions
------------------

1.  Initialize the environment.
2.  Set your target chip: esp32c5, c6, or s3.
3.  Build the project.
4.  Flash and monitor.

``` bash
. ./esp-idf-v5.5.4/export.sh
idf.py set-target esp32c5
idf.py build
idf.py flash monitor
```

Materials
------------------

1.  **Seeed Studios Xiao**: https://www.seeedstudio.com/Seeed-Studio-XIAO-c-2428.html
3.  **Seeed Studios Xiao GPS**: https://www.seeedstudio.com/L76K-GNSS-Module-for-Seeed-Studio-XIAO-p-5864.html
3.  **ElectroCookie Mini PCB**: https://www.amazon.com/dp/B081MSKJJX
4.  **3V Active Buzzer**: https://www.amazon.com/dp/B07VRK7ZPF
4.  **2mm Screws**: https://www.amazon.com/dp/B0CQP76MD8


Assembly
------------------

1. Solder female headers onto the L76K GPS board.
2. Make sure Xiao board is fully seated into the proto board.
2. Solder at least the GND and D9 pins on the Xiao to the protoboard at F2 and F5.
3. Solder the positive leg of the beeper to I5 and negative to I2.
4. Seat L76K board female pints to Xiao board male pins.
5. Wrap GPS antenna wire around antenna and neatly tuck it under proto board, I promise it fits.
6. Screw 4x 2mm screws through proto board corners to lock into base.
7. Redesign top to actually fit buzzer and USB Cables or print top as is and use a dremel and a dream to make it fit.


Mapping Targets
------------------

If using a GPS, you can map the hits later. In the map folder, there is a demo log and python script to generate an index.html file that will map the hits on OpenStreetMap.

``` bash
python3 -m venv env
pip install -r requirements.txt
python map_generator.py demo_log.txt
```
