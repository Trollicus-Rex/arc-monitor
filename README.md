[arcd.c](https://github.com/user-attachments/files/31110790/arcd.c)
[arc_cli.c](https://github.com/user-attachments/files/31110789/arc_cli.c)
[applet.js](https://github.com/user-attachments/files/31110788/applet.js)
# "Covenant" ArcMonitor & Telemetry Daemon

A bare-metal hardware discovery, unprivileged telemetry, and device management suite for **Intel Arc (Alchemist & Battlemage)** GPUs on Linux. Complete with a Cinnamon Desktop Applet themed around Tech Jesus himself.

Because Intel's default Linux tools (stick your tongue on it and guess??) didn't give us the hardware data we wanted, we built our own. 

## 🌟 Features

* **`arcd` (The Daemon):** A zero-polling, D-Bus driven system daemon. It talks directly to the DRM/Xe driver and Intel Level Zero Sysman APIs to pull telemetry without wasting CPU cycles.
* **`arc-cli` (The Client):** An unprivileged CLI tool to query your Arc GPU or send commands (like adjusting Power Limits or Fan PWM) over D-Bus.
* **Steve ArcMonitor (Cinnamon Applet):** A dynamic, reactive desktop applet. Steve reacts in real-time to your GPU's states:
  * 🟢 **Normal:** Steve approves.
  * 🟡 **VRAM > 90%:** Steve is judging your texture settings.
  * 🟠 **Fan > 2000 RPM:** Steve's hair is blowing in the wind (automatically expands the taskbar).
  * 🔴 **Temp > 85°C:** Steve is sweating (triggers a desktop warning notification).
  * 📊 **Dynamic Bars:** Auto-hiding progress bars for Compute and Video Decode engines.

## 📦 Dependencies

To build and run the daemon and CLI, you need the following packages on Ubuntu/Linux Mint:

**Build Dependencies:**
```bash
sudo apt update
sudo apt install build-essential cmake pkg-config libsystemd-dev
