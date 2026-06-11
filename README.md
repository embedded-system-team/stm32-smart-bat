# Smart Bat Swing Game

Smart Bat is an embedded interactive swing game project. The STM32 board reads IMU motion data, detects swing events, and sends them through UDP or serial logs. A Python bridge receives those events and forwards them as JSON, and the Godot 3D game uses the swing timing and speed to trigger hits, scoring, and visual feedback.

This milestone is a playable demo: a real development board can be swung like a bat, the swing data is sent to the host computer, and the Godot scene reacts with batting animation, impact effects, score, and result feedback.

## Project Structure

```text
smart-bat/
├── firmware/          STM32 firmware project
├── host/              Python bridge, test tools, and earlier pygame demos
├── godot_smartbat/    Godot 4.6 3D swing game
└── README.md
```

## System Overview

```text
STM32 + LSM6DSL IMU
        |
        | Wi-Fi UDP or UART logs
        v
host/swing_bridge.py
        |
        | UDP JSON 127.0.0.1:4242
        v
Godot Smart Bat 3D
```

The firmware emits `SWING_START` and `SWING_END` events with timing, peak angular velocity, estimated bat speed, CMSIS-DSP metrics, filtered peak values, and dropped sample counts. `swing_bridge.py` parses those fields and forwards a JSON packet to Godot.

## Features

- Swing detection using STM32L475 and the onboard LSM6DSL IMU
- FreeRTOS tasks for sensor sampling, communication, Wi-Fi, and UDP handling
- CMSIS-DSP metrics including RMS, energy, peak, mean, standard deviation, and FIR-filtered values
- UDP and UART paths for sending swing events to the host computer
- Godot 4.6 3D batting scene with pitching, swing animation, impact effects, and score display
- Python test utilities for UDP, pitch synchronization, and serial debugging

## Hardware Requirements

- B-L475E-IOT01A1 or a compatible STM32L475 development board
- Onboard LSM6DSL IMU
- Wi-Fi network shared by the STM32 board and host computer
- Host computer capable of running Python and Godot

## Software Requirements

- ARM GNU Toolchain, such as `arm-none-eabi-gcc`
- `make`
- Python 3
- Godot 4.6
- `pyserial` when using the UART bridge

```bash
python3 -m pip install pyserial
```

## Wi-Fi Configuration

Do not commit real Wi-Fi credentials to GitHub. Create a local credentials header from the template:

```bash
cp firmware/Core/Inc/wifi_credentials.h.template firmware/Core/Inc/wifi_credentials.h
```

Then edit `firmware/Core/Inc/wifi_credentials.h`:

```c
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define SERVER_IP {192, 168, 0, 100}
#define SERVER_PORT 5005
#define STM32_UDP_PORT 5006
```

`SERVER_IP` should be the IP address of the host computer running the Python bridge. `.gitignore` already excludes `firmware/Core/Inc/wifi_credentials.h`, but it is still worth checking `git status` before publishing.

## Build the Firmware

```bash
cd firmware
make
```

After a successful build, the output files are generated in `firmware/build/`:

- `firmware.elf`
- `firmware.hex`
- `firmware.bin`

Flash the firmware with your preferred STM32 workflow, such as STM32CubeProgrammer, OpenOCD, ST-LINK Utility, or an IDE-integrated flashing tool.

## Run the Godot Game

Open this project with Godot 4.6:

```text
godot_smartbat/project.godot
```

The main scene is `godot_smartbat/main.tscn`. The game listens for swing events on local UDP port `4242`.

Controls:

- Swing after `PITCH!` appears
- `SPACE`: next pitch
- `ESC`: quit

## Run the Python Bridge

### UDP Mode

When the STM32 board and host computer are on the same Wi-Fi network, UDP mode is the recommended path:

```bash
python3 host/swing_bridge.py --source udp
```

Default ports:

- Host receives STM32 events on `0.0.0.0:5005`
- STM32 local UDP port is `5006`
- Bridge forwards events to Godot at `127.0.0.1:4242`

To specify the STM32 IP address manually:

```bash
python3 host/swing_bridge.py --source udp --stm32-ip 192.168.0.159
```

### UART Mode

To read debug logs from the ST-LINK serial port:

```bash
python3 host/swing_bridge.py --source serial
```

You can also specify the serial port manually:

```bash
python3 host/swing_bridge.py --source serial --serial-port /dev/ttyACM0
```

Common ports are `COM3` on Windows and `/dev/ttyACM0` or `/dev/ttyUSB0` on Linux.

## Test Utilities

The `host/` directory contains helper scripts:

- `test_udp_recv.py`: receive UDP packets
- `test_udp_ping.py`: send `PING` to the STM32 board
- `test_udp_pitch.py`: test UDP pitch commands
- `test_pitch_sync.py`: test serial pitch synchronization
- `swing_reader.py`: read and parse serial swing events
- `swing_game_udp.py`, `swing_game_sync.py`, `swing_game_fake3d.py`: earlier pygame demos and synchronization tests