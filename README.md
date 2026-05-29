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

1.  **Initialize the environment**: . /path/to/esp-idf-v5.5.2/export.sh
2.  **Set your target chip**: idf.py set-target esp32c5 (or esp32c6 / esp32s3)
3.  **Build the project**: idf.py build
4.  **Flash and monitor**: idf.py flash monitor
