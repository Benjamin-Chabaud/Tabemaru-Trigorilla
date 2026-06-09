# Tabemaru Control Board

## Overview
This repository contains the firmware (`tabemaru-V5-verbose.ino`) and documentation for the **Tabemaru Control Board**. The system is built around an Arduino Mega 2560 architecture (specifically using an Anycubic Trigorilla motherboard) combined with a custom PCB.

The system is designed to automate a sequence involving valves, a motor, and a heated bed, while continuously logging environmental data (temperature and humidity) to both a Serial Monitor and an SD Card in a non-blocking manner.

## Firmware Logic Explanation
The firmware is built using a non-blocking architecture, utilizing `millis()` to manage multiple tasks simultaneously without halting the processor. 

### 1. The Operational Sequence
The core logic of the machine relies on a 4-step state machine triggered by a hardware button (Button X-).
*   **Step 1 (Valve 1):** The sequence begins, the hotbed turns on, and Valve 1 opens for `1m 40s`.
*   **Step 2 (Motor):** Valve 1 closes, and the Motor runs for `5s`.
*   **Step 3 (Valve 2):** The motor stops, and Valve 2 opens for `2m 20s`.
*   **Step 4 (Wait & Hotbed):** Valve 2 closes. The system waits until the total elapsed time reaches `800s`. During this entire 800-second sequence, the heated bed actively regulates its temperature. Once 800 seconds pass, the sequence automatically terminates and resets.

*Note: All timings are defined as constants at the top of the code and can be easily modified.*

### 2. Hardware Interrupts
To ensure zero-latency responsiveness, the physical buttons do not use standard `digitalRead()` polling. Instead, they use Hardware Interrupts (`attachInterrupt`). 
*   **X- Button (Sequence Start/Stop):** Reacts instantly to start the sequence or act as an emergency stop.
*   **Z- Button (LED / SD Eject):** A short press toggles the internal LED. Holding the button for 1.5 seconds safely ejects the SD card (suspends writing) or remounts it.

### 3. SD Card & Data Storage
The system logs data in CSV format. It features a robust **Hot-Swap background task**. If the SD card is removed (or communication fails due to interference), the system will safely catch the error and attempt to re-initialize the SD card every 10 seconds. When the card is reinserted, data logging resumes automatically.

**SD Card Requirements & Formatting:**
For the SD card module to work reliably with the Arduino architecture, the following requirements MUST be met:
*   **Capacity:** 8GB is highly recommended for maximum stability (though up to 32GB SDHC is technically supported). SDXC cards (64GB+) will not work.
*   **File System:** The card MUST be formatted in **FAT32** (or FAT16). exFAT, NTFS, or APFS are not supported by the standard Arduino library.
*   **Formatting Tool & Deep Reset:** It is strongly advised to use the official SD Association's *SD Memory Card Formatter*. If the Arduino fails to initialize the card, a standard "Quick Format" is often not enough. You must select **"Overwrite Format"** (Full Format) to completely wipe the flash memory sectors and rebuild the file system from scratch.
*   **Partition Scheme:** The card's partition map MUST be **MBR (Master Boot Record)**. If the card was previously formatted on a Mac or newer OS as GPT (GUID Partition Table), the Arduino will fail to read it. To fix this, the partition table must be completely wiped (e.g., using `diskpart > clean` on Windows) before reformatting to FAT32.
*   **File Naming:** The system writes to `LOG.CSV`, strictly adhering to the legacy 8.3 filename convention.

**Data Storage Capacity & Retention:**
At the current logging rate (one entry every 2 seconds), the system generates approximately 1.2 GB of CSV data per year of continuous 24/7 operation. An 8GB SD card (the recommended maximum size for optimal compatibility) can hold data for **over 5 years** before running out of space. 

Depending on your operational needs, you can either:
*   Safely eject the SD card every few years to manually archive and delete the data.
*   Modify the logging function in the firmware to implement a rolling log (automatically deleting the oldest data when space runs out).

**Hardware & Operational Recommendations:**
*   **Logic Level Shifting:** The Arduino outputs 5V logic, but SD cards strictly require 3.3V. Ensure your SD card reader module has a built-in logic level converter chip and a 3.3V voltage regulator. Direct 5V connections will damage the SD card and cause instability.
*   **Cable Length & Interference:** The SPI protocol is highly susceptible to electromagnetic interference (EMI). Keep the SPI cables (MISO, MOSI, SCK) as short as possible and route them away from high-power cables (like the hotbed heater or motor).
*   **Flash Wear-Leveling:** Since the system writes data continuously (24/7), standard consumer SD cards may suffer from premature flash memory degradation. It is highly recommended to use a **"High Endurance"** SD card (typically used for dashcams or security cameras) designed for continuous write operations.

### 4. Environmental Monitoring & Thermostat
*   **DHT20 Sensors:** Two I2C temperature/humidity sensors are routed through a **TCA9548A Multiplexer**. They are polled every 2 seconds.
*   **Thermostat:** The system reads an NTC 100k thermistor connected to analog pin `A15` (Port T2) and uses the Steinhart-Hart equation to calculate temperature in Celsius. It maintains a target temperature of 105°C using a 1°C hysteresis.
*   **OLED Display:** A 128x32 OLED display provides real-time feedback on sequence state, actuator status, SD card health, and sensor readings.

---

## Custom PCB Analysis (KiCad)

The custom "Tabemaru control board" acts as an intermediate shield/routing board that neatly bridges the Trigorilla motherboard with the external peripherals, and also acts as a Human-Machine Interface (HMI).

### Schematic Details
1.  **I2C Multiplexer (TCA9548A):**
    *   Connectors `J1` and `J6` are designed to host a TCA9548A multiplexer breakout board. 
    *   The `sda` and `scl` lines from the Trigorilla input are fed into the multiplexer.
    *   The multiplexer outputs channels `sd1/sc1` and `sd2/sc2` to split the I2C bus, allowing two identical DHT20 sensors (which have the same hardcoded I2C address) to operate simultaneously.
2.  **DHT20 Sensor Headers:**
    *   `J2` and `J3` connect to the Top and Bottom DHT20 sensors. They receive the separated I2C channels from the multiplexer alongside VCC and GND.
3.  **Trigorilla Interconnect (`J4`):**
    *   This 6-pin connector bridges the custom PCB back to the main Trigorilla board. It carries the primary I2C bus (`sda`, `scl`), Ground, VCC, and the two button signals (`bp1`, `bp2`).
4.  **OLED Screen Header (`J5`):**
    *   This 10-pin header routes `VCC`, `GND`, `sda`, and `scl` directly to the OLED screen. Notes indicate that `sc1` and `sd1` are connected just to physically route them through the connector, though they are not utilized by the screen itself.
5.  **User Inputs (Buttons):**
    *   `SW1` and `SW2` are tactile push buttons that pull the `bp1` and `bp2` lines to Ground when pressed. These correspond to the X- and Z- endstop logic in the code.
6.  **Power Delivery & Decoupling:**
    *   Capacitors `C1`, `C2`, `C3`, `C4` are placed across the VCC and GND lines to stabilize the power delivery to the sensors and ICs, preventing voltage drops and filtering out electrical noise.

### PCB Layout Notes
*   The PCB is a two-layer board (Top Copper in Red, Bottom Copper in Blue). Both sides are identical and interconnected so that the connectors can be mounted on either side depending on the final position of the board inside the composter. Although this makes the routing technically less optimized (for instance, a VCC line making a half-loop is not ideal), it provides necessary mechanical flexibility for prototyping.
*   Thick traces are used for the main VCC and GND rails to ensure stable current delivery, while thinner traces are used for data lines (`sda`, `scl`).
*   The design elegantly centralizes all messy external wiring (buttons, screen, sensors) into a single, clean interface board that connects directly to the Trigorilla via the `J4` header.
