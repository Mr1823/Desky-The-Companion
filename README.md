# Desky — The Companion 🐾

A smart desk companion pet powered by the **NodeMCU ESP-12E (ESP8266)**. Desky features an animated expressive face on one OLED, an interactive information dashboard on a second OLED, RFID-based work/break mode switching, physical servo gestures, hydration reminders, and customizable session timing.

---

## ✨ Features

- **Dual OLED Displays (I2C single bus):**
  - **Screen A (0x3C):** Animated expressive face (idle, happy, focused, loving, sleepy, surprised, wink, big grin).
  - **Screen B (0x3D):** Dashboard displaying live IST clock (NTP synced), active mode, session timer, progress bar, and session summaries.
- **RFID Mode Switching (RC522):**
  - Tap Card #1 to start a **WORK** session.
  - Tap Card #2 to start a **BREAK** session.
  - Quick card learning on first boot.
- **Physical Gestures (SG90 Servo):**
  - Head nods on session start and card taps.
  - Happy wiggles on button interactions and breaks.
  - Gentle nudge for hydration reminders.
  - Auto-detaches when idle to eliminate servo jitter and prevent brownouts.
- **Hydration Nagging:**
  - Alerts you every 20 minutes during work sessions to drink water with an on-screen banner, heart eyes, and a physical servo nudge.
- **Interaction (Push Button):**
  - Press to play with Desky and trigger spontaneous animated reactions.
- **Session Dial (Potentiometer on A0):**
  - Adjust session duration on the fly from 5 to 60 minutes (5-minute increments).

---

## 🔌 Pin Map (NodeMCU ESP-12E)

| Function | NodeMCU Pin | GPIO | Target Component | Notes |
|---|---|---|---|---|
| **I2C SDA** | `D2` | GPIO4 | Both OLEDs (SDA) | Shared I2C bus |
| **I2C SCL** | `D1` | GPIO5 | Both OLEDs (SCL) | Shared I2C bus |
| **SPI SCK** | `D5` | GPIO14 | RC522 (SCK) | RFID clock |
| **SPI MISO** | `D6` | GPIO12 | RC522 (MISO) | RFID data out |
| **SPI MOSI** | `D7` | GPIO13 | RC522 (MOSI) | RFID data in |
| **RFID SS / SDA** | `D4` | GPIO2 | RC522 (SDA / NSS) | Chip select |
| **RFID RST** | `3V3` | — | RC522 (RST) | Tied to 3.3V (always on) |
| **Servo Signal** | `D8` | GPIO15 | SG90 Servo (PWM) | Signal wire |
| **Button** | `D3` | GPIO0 | Push Button | `INPUT_PULLUP` to GND |
| **Session Dial** | `A0` | ADC0 | Potentiometer wiper | 0 - 3.3V |

> [!TIP]
> Add a **470 µF electrolytic capacitor** across the 5V power rail (VIN to GND) to buffer current spikes when the servo moves and prevent board resets.

---

## 📁 Project Structure

```text
Desky-The Companion/
├── platformio.ini              # PlatformIO environment config
├── README.md                   # Project documentation
├── .gitignore                  # Git ignore rules (credentials protected)
├── src/                        # PlatformIO firmware source
│   ├── main.cpp                # Core Desky firmware
│   ├── Animation.h             # 16 expressions × 6 frames (bitmap art)
│   ├── credentials_example.h   # Wi-Fi template for public repos
│   └── credentials.h           # (Private / Git-ignored) Your Wi-Fi info
└── desky/                      # Arduino IDE standalone sketch
    ├── desky.ino               # Arduino sketch file
    ├── Animation.h             # Face art header
    ├── credentials_example.h   # Wi-Fi template
    └── credentials.h           # (Private / Git-ignored) Your Wi-Fi info
```

---

## ⚙️ Configuration & Credentials

To keep your Wi-Fi credentials private:

1. Copy `credentials_example.h` to `credentials.h` inside `src/` (for PlatformIO) or `desky/` (for Arduino IDE):
   ```c
   const char *WIFI_SSID = "YOUR_WIFI_NAME";
   const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
   ```
2. `credentials.h` is already listed in `.gitignore` so your private details will never be committed to GitHub.

---

## 🚀 How to Flash

### Option A: Using PlatformIO (Recommended)

1. Connect your NodeMCU to your computer via micro-USB.
2. Build the project:
   ```bash
   pio run
   ```
3. Flash the firmware:
   ```bash
   pio run -t upload
   ```
4. Open the Serial Monitor (115200 baud):
   ```bash
   pio device monitor
   ```

### Option B: Using Arduino IDE

1. Open **Arduino IDE**.
2. Go to **Settings / Preferences** and ensure the ESP8266 board manager URL is added:
   ```text
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
3. Open `desky/desky.ino`.
4. Install the required libraries via **Tools > Manage Libraries**:
   - `U8g2` by oliver
   - `MFRC522` by GithubCommunity
   - `Servo` (built into ESP8266 core)
5. Select Board: **NodeMCU 1.0 (ESP-12E Module)**.
6. Select your Port and click **Upload**.

---

## 🎮 First Boot & Card Learning

1. Desky boots, displays a splash face, connects to Wi-Fi, and syncs IST time via NTP.
2. **Card Learning:**
   - Tap your first RFID card → Assigned as **WORK** card.
   - Tap your second RFID card → Assigned as **BREAK** card.
3. Once learned, tapping the cards immediately switches Desky between Work, Break, and Idle modes!
