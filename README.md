# HeyClawy

HeyClawy is a voice-first ESP32 companion for [OpenClaw](https://github.com/openclaw/openclaw).  
You talk to the device, it sends your request to OpenClaw in real time, and speaks the response back.

## Supported Devices (Current 3)

### 1) SenseCAP Watcher
![SenseCAP Watcher](img/SenseCAP.JPG)

### 2) M5StickC Plus2
![M5StickC Plus2](img/M5Stick.JPG)

### 3) [Waveshare ESP32-S3 Audio Board](https://www.waveshare.com/esp32-s3-audio-board.htm)

![WaveShare Audio Board](img/WaveShare_Audio_Board.PNG)


### Live Demo (SenseCAP)
![SenseCAP Live Demo](img/SenseCap_Demo.gif)

## Main Features

- Voice commands feel natural: press once (or use wake word), speak, get a spoken response.
- Wake word support is built in (default WakeNet model is `Hey Jarvis`).
- Real-time OpenClaw integration over WebSocket:
  - chat responses
  - live task updates
  - cron notifications
  - status and activity data
- Works with both display devices and a screenless audio board.
- Built-in local web settings page for quick tuning without reflashing every time.

## Requirements

### Development machine

- Windows (batch scripts included), Linux, or macOS
- ESP-IDF `v5.5+` (use vscode esp-idf extension)
- USB cable for flashing
- One of the supported devices (SenseCAP Watcher, M5StickCPlus2, Waveshare Audio Board)

### OpenClaw machine

- OpenClaw gateway running
- STT service (faster-whisper HTTP endpoint, default port `5051`)
- TTS service (OpenAI-compatible EdgeTTS endpoint, default port `5050`) 

For the intended network layout, see [docs/openclaw-network-model.md](docs/openclaw-network-model.md).
The short version is:

- OpenClaw gateway stays loopback-only on the host
- Tailscale exposes the remote control path
- ESP32-facing helpers can stay on the LAN as separate services

### Simple EdgeTTS installation on the OpenClaw machine

Run:

```bash
docker run -d --name openclaw-edgetts --restart unless-stopped -p 5050:5050 travisvn/openai-edge-tts
```

Quick test:

```bash
curl -X POST "http://127.0.0.1:5050/v1/audio/speech" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"tts-1\",\"voice\":\"alloy\",\"input\":\"Hello from HeyClawy\"}" \
  --output hello.mp3
```

If `hello.mp3` is created, TTS is ready.

## Build (Easy Path)

From project root:

### SenseCAP Watcher
```bat
build_sensecap.bat
```

### M5StickC Plus2
```bat
build_m5stick.bat
```

### Waveshare ESP32-S3 Audio Board
```bat
build_audio_board.bat
```

Then flash:

```bat
set ESPPORT=COM3
idf.py flash monitor
```

Replace `COM3` with your actual serial port.

## Configuration

### 1) Create local secrets files

- Copy `secrets_example.txt` to `secrets.txt`
- Copy `main/include/secrets.h.example` to `main/include/secrets.h`

### 2) Fill required values

At minimum:

- WiFi SSID/password
- OpenClaw host/port/token
- Device key (`SECRETS_DEVICE_KEY_HEX`)
- TTS host/port/api key/voice
- STT host/port

### 3) Generate/pair device key (recommended)

```bash
python tools/test_openclaw_auth.py
```

Use the printed private key hex in `main/include/secrets.h`, then approve the device in OpenClaw.

### 4) Optional runtime tuning

After boot, you can update settings from the device web UI (audio, thresholds, behavior, services, etc.).

## Caveats and Disclaimers

- This project is under active development. Expect rough edges.
- Keep all secrets local. `secrets.txt` and `main/include/secrets.h` must never be committed.
- Wake word quality depends on mic conditions and background noise.
- M5StickC Plus2 uses a passive buzzer, so voice playback quality is lower than codec-based boards.
- You are responsible for securing your OpenClaw and LAN environment.
