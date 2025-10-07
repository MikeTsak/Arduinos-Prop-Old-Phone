# Old Phone Prop (ESP8266 + DFPlayer + Rotary Dial)

Turn an old rotary-dial telephone into a **network-controlled prop** for LARP or theater.  
This sketch runs on an ESP8266 (NodeMCU / Wemos D1 mini), controls a real telephone ringer,  
plays audio tracks from an SD card via DFPlayer Mini, and even reads digits from the rotary dial.

---

## ✨ Features

- **Wi-Fi control panel** (mobile-friendly dark/light UI)
  - Ring / Stop ring / Beep test
  - Volume slider + presets
  - Track buttons for the first 5 “named” sounds
  - Auto-generated buttons for other tracks on the SD card
  - LED on/off/toggle
  - Shows current hook state, ringing status, IP address, and dialed digits

- **Authentic ringing**
  - 2s ON / 4s OFF cadence
  - Two-tone trill for piezo buzzers
  - Or PWM drive for external ringer (12–50 V via relay or transistor)

- **Audio playback**
  - DFPlayer Mini reads MP3 files from SD
  - First 5 tracks have fixed names:
    1. Dial (loops)  
    2. Calling (loops)  
    3. End Call (loops)  
    4. Voicemail (play once)  
    5. Number doesn’t exist (play once)
  - Tracks >5 are shown as generic “Track N” buttons

- **Rotary dial input**
  - Reads pulses from the dial and assembles digits
  - Shows last digit and dial buffer in the web UI
  - Can trigger tracks automatically when off-hook

- **LED indicators**
  - Onboard LED blink-codes the last octet of the IP at boot
  - LED controllable from the web UI

---

## 🛠 Hardware Needed

- **ESP8266 board** (Wemos D1 mini or NodeMCU recommended)
- **DFPlayer Mini** + microSD card (FAT32, 128 kbps MP3 works well)
- **Speaker or old handset earpiece** for audio
- **Piezo buzzer / passive piezo disc** OR
- **Relay module** (5 V, optocoupled) to switch a **12–50 V telephone ringer**
- **Old rotary dial phone**
  - Hook switch contact → D7 + GND
  - Rotary dial pulse contacts → D2 + GND
  - Rotary dial off-normal contacts (optional) → D1 + GND

**Power:**
- ESP8266: 5 V USB
- DFPlayer: 5 V (can share with ESP 5 V pin)
- Ringer: external 12–50 V supply (isolated by relay)

---

## 📐 Wiring Summary

- **ESP D5 (TX)** → DFPlayer RX  
- **ESP D6 (RX)** ← DFPlayer TX (optional, status)  
- **ESP D7** → hook switch (other side → GND)  
- **ESP D2** → dial pulse contact (other side → GND)  
- **ESP D1** → dial off-normal contact (other side → GND)  
- **ESP D8** → piezo / transistor base / relay IN  
- **Speaker** → DFPlayer SPK_1 / SPK_2  
- **SD card** in DFPlayer with `0001.mp3 … 9999.mp3`

---

## 🔌 Operating Modes

Set `RING_MODE` at the top of the sketch:

- `RING_MODE_PIEZO` – passive piezo or active buzzer on D8
- `RING_MODE_TRANSISTOR` – low-voltage transistor switching ~12 V
- `RING_MODE_RELAY_HV` – relay module on D8 switching external 12–50 V

---

## 🌐 Web Interface

After boot, connect to the same Wi-Fi network as the ESP8266.  
Visit the ESP’s IP in your browser (shown in Serial Monitor, or blink-coded on LED).

UI cards include:
- **Status:** IP, hook state, ringing, LED state, track count, dial buffer
- **Phone Control:** Ring/Stop, Beep, Stop Play, Rescan SD
- **Volume:** slider (0–30) + presets
- **LED:** On / Off / Toggle
- **Tracks:** buttons for first 5 named sounds + auto-generated extras

---

## 📝 Dialing Behavior

- Dialing digits generates pulses (1=1 pulse … 0=10 pulses).
- Off-hook: digits can directly select tracks (0 → track 10).
- Buffer shown in UI, can be cleared.

---

## 🔊 Default Behavior

- At boot, LED blinks last octet of IP until user changes LED.
- When handset goes **off-hook** and no track chosen:  
  → **loops Track 1 (Dial) at volume 12**.
- If user selected a track:  
  → plays that track (loop or once depending on ID).

---

## ⚠️ Safety Notes

- If you drive a **real 50 V ringer**, **always use a relay or SSR**.  
- Do **not** connect 50 V directly to ESP pins or small BJTs like D311.  
- Add a fuse (250–500 mA fast) in series with the high-voltage line.  
- Double-check polarity and wiring before powering.

---

## 📂 File Naming

Put files on the SD card as:

```
/0001.mp3 (Dial tone loop)
/0002.mp3 (Calling loop)
/0003.mp3 (End Call loop)
/0004.mp3 (Voicemail once)
/0005.mp3 (Number doesn’t exist once)
/0006.mp3 ...
```


DFPlayer requires **4-digit names** in the root folder.
