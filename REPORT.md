# Smart Bat: An Embedded Motion-Controlled Batting System

## Abstract

Smart Bat is an embedded motion-controlled batting system built around an STM32 IoT node. A B-L475E-IOT01A1 board is mounted on a bat-like handle and uses the onboard LSM6DSL accelerometer and gyroscope to detect real swing motion. The firmware sends swing events to a host computer through Wi-Fi UDP or UART logs. A Python bridge parses the firmware events, converts them into JSON packets, and forwards them to a Godot 3D game, where swing timing, angular velocity, estimated bat speed, and DSP features are used to judge hits, misses, and swing scores.

The project integrates several technologies emphasized in class: STM32 IoT node development, sensor input, GPIO output devices, Wi-Fi wireless communication, UART, DMA, FreeRTOS/CMSIS-RTOS APIs, multitasking, and CMSIS-DSP signal processing. In addition to visual feedback in the game, the bat side also drives a vibration motor to provide immediate haptic feedback when a swing is confirmed.

## Motivation

Most baseball games are controlled with a keyboard, mouse, or controller, so the player's real swing is not directly connected to the batting result. Our goal was to turn a physical swing into an interactive game event and reflect both swing timing and swing strength in a 3D batting scene.

The project goals are:

- Convert real bat motion into game events for motion-controlled entertainment.
- Use an STM32 IoT node for real-time sensing, wireless transmission, and output feedback.
- Practice hardware-software integration between an embedded system and a PC game engine.

## System Architecture

```text
STM32 Smart Bat
  B-L475E-IOT01A1 + LSM6DSL IMU
  FreeRTOS tasks
  real-time swing detection
  CMSIS-DSP swing analysis
  vibration motor feedback
        |
        | Wi-Fi UDP events or UART debug logs
        v
PC Bridge
  host/swing_bridge.py
  parse SWING_START / SWING_END
  convert to JSON
        |
        | UDP JSON 127.0.0.1:4242
        v
Godot 3D Game
  godot_smartbat/main.tscn
  pitch, hit judgment, swing score, visual feedback
```

The bat side is responsible for real-time sensing, initial swing detection, event generation, and vibration feedback. The PC bridge receives events, converts them into a game-friendly format, and forwards them. The Godot side handles the 3D scene, pitch animation, hit judgment, scoring, and user feedback. This split keeps the firmware real-time and simple, while allowing the game logic and scoring model to be adjusted quickly on the PC.

The STM32 firmware separates IMU data into two paths:

- Real-time detection path: evaluates samples one by one to trigger swing start/end with low latency.
- DSP analysis path: collects gyro magnitude during a swing and computes RMS, energy, peak, and other features after the swing ends.

This design means CMSIS-DSP does not participate in the `SWING_START` trigger decision, so adding motion analysis does not increase swing detection latency.

### Data Flow

1. The STM32 periodically reads 3-axis acceleration and 3-axis angular velocity from the LSM6DSL.
2. `sensorTask` pushes IMU samples into a FreeRTOS message queue.
3. `commTask` consumes samples from the queue and detects swing start, peak, and end using thresholds and a state machine.
4. The firmware emits `SWING_START` and `SWING_END`. `SWING_END` includes timestamps, peak angular velocity, estimated bat speed, and CMSIS-DSP metrics.
5. `udpTask` sends events to the PC through Wi-Fi UDP. UART debug logs are also available as a fallback path.
6. `host/swing_bridge.py` parses the event and reconstructs the peak time from `t`, `peak_t`, and `duration`.
7. The bridge sends a JSON packet containing `peak_age_ms`, speed, and DSP metrics.
8. Godot receives the JSON through non-blocking `PacketPeerUDP` and judges the result using timing, strength, and data quality.

## Hardware and Software

### Hardware

- STM32 B-L475E-IOT01A1 IoT node
- Onboard LSM6DSL 6-axis IMU
- Onboard ISM43362 Wi-Fi module
- Vibration motor module controlled through GPIO
- A PC or laptop on the same Wi-Fi network as the STM32 board

### Software

- STM32 HAL and BSP drivers
- FreeRTOS / CMSIS-RTOS V2
- CMSIS-DSP
- ARM GNU Toolchain and Makefile
- Python 3
- Godot 4.6
- pyserial for UART bridge mode

## Firmware Design

### Multitasking Architecture

The firmware uses CMSIS-RTOS V2 APIs to create multiple tasks and separate sensing, communication, Wi-Fi, and UDP handling:

- `sensorTask`: initializes the LSM6DSL, calibrates gyroscope bias, and periodically reads IMU samples.
- `commTask`: handles UART/DMA receive events, PITCH synchronization, swing detection, vibration motor control, and swing event output.
- `wifiTask`: initializes Wi-Fi, scans access points, connects to the configured network, and obtains an IP address.
- `udpTask`: opens the UDP client, sends `HELLO_FROM_STM32`, receives `PING`/`PITCH`, and transmits queued swing events.
- `defaultTask`: reserved for background system work.

Tasks communicate through message queues and thread flags. IMU samples are passed to `commTask` through `imuQueueHandle`; UDP messages are buffered through `udpTxQueueHandle` so that swing detection is not blocked by Wi-Fi transmission.

### Swing Detection

Swing detection is based on the magnitude of 3-axis angular velocity and acceleration:

```text
gyro_mag2 = gx^2 + gy^2 + gz^2
acc_mag2  = ax^2 + ay^2 + az^2
```

The state machine has four states:

- `IDLE`: waits for angular velocity and acceleration to exceed the start thresholds.
- `CANDIDATE`: confirms that the motion remains above the threshold, reducing false triggers caused by short noise spikes.
- `SWINGING`: records swing data and continuously updates the peak time and peak angular velocity.
- `COOLDOWN`: prevents the same physical swing from being detected multiple times.

When a swing candidate remains valid for `SWING_CONFIRM_MS`, the firmware emits `SWING_START`. When angular velocity drops below the end threshold and the duration exceeds the minimum swing duration, it emits `SWING_END`.

### CMSIS-DSP Analysis

To avoid relying on a single peak value, the firmware records gyro magnitude during each swing and uses CMSIS-DSP to compute:

- RMS angular speed
- Energy index
- Peak, mean, standard deviation, and minimum
- FIR-filtered RMS, energy, and peak

These fields are included in the `SWING_END` message:

```text
SWING_END t=... dur=... peak_t=... peak_dps=... speed_x100=...
rms_dps=... energy=... cmsis_peak=... mean_dps=... std_dps=...
filt_rms=... filt_energy=... filt_peak=... dsp_n=...
```

Godot uses these metrics for scoring, so the result reflects both timing and swing strength. The real-time peak and the CMSIS-DSP `arm_max_f32` peak can also validate each other; when they are close, the state-machine peak tracking and post-swing DSP analysis are consistent.

### FIR Filtering

In addition to raw gyro magnitude, the project applies a 5-tap moving average FIR filter to reduce short noise spikes:

```text
h = [0.2, 0.2, 0.2, 0.2, 0.2]
filtered[n] = 0.2 * (x[n] + x[n-1] + x[n-2] + x[n-3] + x[n-4])
```

The filtered signal is used to compute:

- `filt_peak`
- `filt_rms`
- `filt_energy`

These filtered metrics reduce the influence of single-sample spikes on peak and energy, making the Godot score more stable.

### Vibration Motor Feedback

The bat side uses a GPIO pin to control a vibration motor. The motor is turned on when `SWING_START` is confirmed, simulating an impact-like feedback moment. To avoid interfering with IMU sampling, the motor pulse is non-blocking:

- When the motor turns on, the firmware records a timestamp.
- Each IMU-processing loop checks whether the pulse duration has expired.
- When time is up, the GPIO is turned off without using a blocking delay.

During testing, a short 45 ms pulse was too weak because the motor rotor did not have enough time to start. Increasing the pulse duration produced a clear haptic response.

### UART DMA + IDLE Receive

Early UART receive logic based on polling or blocking reads could consume CPU time and cause packet drops when IMU sampling was busy. The project improved this by using `HAL_UARTEx_ReceiveToIdle_DMA`:

- DMA moves UART data without occupying the CPU.
- The IDLE interrupt indicates that a line or packet has finished arriving.
- The DMA half-transfer interrupt is disabled to reduce unnecessary interrupts.

This makes UART debug and control-message handling more stable and follows the real-time system principle of avoiding blocking work in the critical path.

## PC Bridge

`host/swing_bridge.py` supports two input sources:

- UDP: receives events sent by the STM32 over Wi-Fi.
- Serial: reads events from ST-LINK UART debug logs.

The bridge parses key-value fields from `SWING_START` and `SWING_END`, reconstructs the swing peak time, computes `peak_age_ms`, and forwards a JSON packet to Godot:

```json
{
  "type": "swing",
  "peak_age_ms": 25.0,
  "peak_dps": 720,
  "speed": 14.2,
  "duration": 180,
  "rms_dps": 410,
  "energy": 83000,
  "filt_rms_dps": 390,
  "filt_energy": 77000
}
```

This bridge keeps Godot-specific formatting out of the firmware and makes debugging, logging, and packet-format changes easier on the PC side.

## Godot 3D Game

The Godot side implements a simplified 3D batting scene:

1. Wait for the next pitch.
2. Display `PITCH!` and move the ball toward the hitting zone.
3. Receive swing JSON packets from the bridge using non-blocking UDP.
4. Use `peak_age_ms` to recover the swing peak time and compare it with the ideal hit time.
5. Compute result and score from timing, peak, RMS, energy, and standard deviation.
6. Display batting animation, ball flight, impact effects, score, and result text.

Godot uses UDP because it is simple, low-latency, and easy to connect to the Python bridge. Keeping graphics and hit judgment on the PC also makes the gameplay easier to tune without reflashing firmware.

### Score Calculation

The score is not only based on whether the bat contacts the ball. It combines timing and motion features:

- Timing: 40%, because batting timing is the core factor.
- Peak: 25%, representing instant explosive strength.
- RMS: 18%, representing effective strength over the full swing.
- Energy: 10%, representing relative motion energy.
- Std: 7%, representing velocity variation and acceleration characteristics.

This makes the feedback closer to motion analysis rather than a simple hit/miss button game.

## Results

The current system achieves the following:

- The STM32 IoT node reads LSM6DSL IMU data and detects real swing motion.
- The firmware emits `SWING_START`, `SWING_END`, and multiple DSP metrics.
- FreeRTOS tasks separate sensing, communication, Wi-Fi, and UDP work.
- Wi-Fi UDP wireless transmission is supported, with UART serial bridge mode as a debug and fallback path.
- The Python bridge converts STM32 events into JSON packets for Godot.
- The Godot 3D game reacts to swing data, displays hit results, speed, and score.
- The bat provides local low-latency haptic feedback through a vibration motor.
- UDP time alignment has been improved. The bridge reconstructs peak time from STM32 timestamps, so Godot can still judge EARLY / GOOD / LATE under Wi-Fi transmission.
- The firmware builds successfully and generates `firmware.elf`, `firmware.hex`, and `firmware.bin`.

## Core Contributions

1. Real-time swing detection on STM32: IMU + FreeRTOS + state machine for low-latency `SWING_START` / `SWING_END`.
2. CMSIS-DSP swing analysis: peak, RMS, energy, mean, standard deviation, and minimum.
3. FIR filtering for noise reduction: a 5-tap moving average produces filtered peak / RMS / energy.
4. UDP + Godot real-time interaction: swing data is sent to Godot for 3D batting and score display.
5. Sports-analysis-style feedback: each swing shows score, speed, energy, stability, and data quality.

## Demo

Demo video link: TODO: add the YouTube or cloud video link after upload.

Recommended demo video content:

- The STM32 board mounted on the bat, showing wireless swing operation.
- The PC running `host/swing_bridge.py --source udp`, showing received `SWING_START` / `SWING_END` or JSON payloads.
- The Godot game showing `PITCH!`, hit/miss result, speed, score, and visual effects.
- If possible, a close-up of the vibration motor installation or trigger moment.

## Limitations and Challenges

### Wi-Fi Time Alignment

In early Wi-Fi tests, the STM32 and PC bridge could receive `SWING_` events, but Godot sometimes still judged MISS. The UART/USB comparison path was more stable, so the issue was likely caused by Wi-Fi latency, packet ordering, or timestamp alignment.

The current version improves this by including `t`, `peak_t`, `duration`, and `peak_dps` in `SWING_END`. The PC bridge reconstructs the actual peak time from STM32 timestamps and sends `peak_age_ms` to Godot. Therefore, Godot no longer judges timing purely by packet arrival time; it aligns the swing with the actual peak time.

After this improvement, the system can still judge EARLY / GOOD / LATE under Wi-Fi transmission and display complete swing analysis.

Remaining improvement directions:

- Add more detailed timestamp logs on both STM32 and PC.
- Add sequence numbers to each swing event.
- Compare `SWING_START`, `SWING_END`, bridge send time, and Godot receive time.
- Further reduce UDP latency and packet loss, and add better network latency tolerance in Godot.

### Vibration Strength

The current vibration motor is controlled with a simple GPIO pulse. It provides basic haptic feedback, but the strength is not adjustable. PWM control or a dedicated haptic driver could make the vibration strength proportional to swing power.

### Demo-Stage Game Content

The Godot scene demonstrates the core interaction, but graphics, sound, complete levels, continuous scoring, and multiplayer gameplay can still be improved.

## Future Work

- Continue improving Wi-Fi timing by adding sequence numbers and more complete timestamp synchronization.
- Use PWM to control vibration strength based on swing power.
- Replace the motor with a stronger ERM/LRA motor or use a haptic driver such as DRV2605L.
- Add swing-curve visualization, peak markers, and raw-vs-filtered comparisons.
- Use more advanced DSP, such as frequency-domain analysis, FFT, or motion consistency scoring.
- Add personalized calibration so score thresholds adapt to each player's strength.
- Save swing records for long-term training analysis.
- Detect swing direction using gyroscope axis information.
- Improve Godot models, impact particles, sound effects, and UI.
- Add more complete game mechanics, such as combos, levels, leaderboards, or multiplayer.

## Physical Meaning of Swing Metrics

- Peak: `max(gyro_mag)`, representing instant explosive strength.
- RMS: `sqrt(mean(gyro_mag^2))`, representing effective strength during the whole swing.
- Energy: `sum(gyro_mag^2)`, representing relative motion energy and swing power.
- Mean: `mean(gyro_mag)`, representing average angular speed.
- Std: standard deviation, representing velocity variation and acceleration characteristics.
- Min: `min(gyro_mag)`, representing the lowest angular speed during the swing.

## How to Run

### Build the Firmware

```bash
cd firmware
make
```

After a successful build, the generated files are:

- `firmware/build/firmware.elf`
- `firmware/build/firmware.hex`
- `firmware/build/firmware.bin`

### Wi-Fi Configuration

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

### Run the Bridge

UDP mode:

```bash
python3 host/swing_bridge.py --source udp
```

UART mode:

```bash
python3 host/swing_bridge.py --source serial --serial-port /dev/ttyACM0
```

### Run Godot

Open the project with Godot 4.6:

```text
godot_smartbat/project.godot
```

The main scene is:

```text
godot_smartbat/main.tscn
```

## Image and Video TODO

The report can be submitted as text, but the following media would make the result clearer:

- TODO: System architecture diagram, either exported from the slides or redrawn.
- TODO: Physical photo of the Smart Bat, showing the STM32 board and vibration motor placement.
- TODO: Godot gameplay screenshot, preferably showing `PITCH!`, HIT/MISS, score, or speed.
- TODO: Demo video link, added to both this report and README.

## References

- STMicroelectronics, B-L475E-IOT01A1 Discovery kit documentation.
- STMicroelectronics, X-CUBE-MEMS1 and LSM6DSL driver documentation.
- STMicroelectronics, STM32Cube HAL and BSP drivers.
- FreeRTOS Kernel and CMSIS-RTOS V2 documentation.
- Arm CMSIS-DSP documentation.
- Godot Engine 4.6 documentation, especially `PacketPeerUDP`.
- Python documentation: `socket`, `json`, and pyserial.
- DRV2605L haptic driver documentation, used as a future improvement reference.
