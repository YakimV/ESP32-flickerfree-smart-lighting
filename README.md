# ESP32 Flicker-Free Smart Lighting

An ESP32-based lighting controller designed to work with power supplies modified for voltage control via a feedback loop. The microcontroller’s PWM signal adjusts the power supply’s output voltage, ensuring a constant voltage is delivered to the LED strip without load chopping or flickering.

---

## What the System Does

* **PSU Voltage Control:** A PWM signal is fed into the power supply’s feedback loop to adjust its output voltage.
* **Relay control:** Supplies 220 V to the power supply when the light is turned on and cuts off power 10 seconds after the voltage drops to 0% (to prevent the relay from clicking during rapid switching).
* **433 MHz RF reception:** processes signals from wireless buttons or remotes (EV1527) and cycles through brightness levels: `25% → 50% → 75% → 100% → Off`.
* **MQTT:** Receives control commands (brightness values and preset settings) and returns the current status of the channels.
* **WireGuard and Web OTA:** Maintains a tunnel to the network via a broker and hosts a local web page for firmware updates via a `.bin` file.
* **FreeRTOS:** Networking (Wi-Fi, VPN, MQTT) is offloaded to Core 0, while RF receiver polling, PWM, and relays run on Core 1, ensuring that buttons respond without delay even if there are connectivity issues.

Translated with DeepL.com (free version)
