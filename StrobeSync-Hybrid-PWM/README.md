# StrobeSync — Hybrid PWM, Sensor Telemetry & Real-Time Sync

Firmware for **STM32F103** (CMSIS bare-metal C) and **Arduino AVR** (ATmega328P),
with compatible **Python** and **C#** UART host clients.

The design combines six PWM channels, a time-based synchronization signal,
four sampled sensor channels, asynchronous USART telemetry, and a lightweight
serial command protocol. The STM32 and Arduino implementations have distinct
pin assignments and peripheral backends; the host-facing commands are shared.

> **Status: prototype / unverified hardware.** This repository packages the
> supplied sources and includes protocol tests for the Python client. Firmware
> builds, electrical behavior, ISR timing, and end-to-end communication have
> not been verified in this packaging process.

## Components

| Component | Platform | Technology | Key features |
|---|---|---|---|
| STM32 firmware | STM32F103x8 / STM32F103xB | C, CMSIS registers | TIM1/TIM3 hardware PWM, TIM2 software PWM scheduler, ADC1 IRQ, USART1 IRQ |
| Arduino firmware | Uno / classic Nano, ATmega328P 16 MHz | Arduino C++ / AVR registers | Timer0/2 hardware PWM, Timer1 ISR scheduling, ADC IRQ, USART0 IRQ |
| Python client | Windows / Linux / macOS | Python 3.9+, pyserial | Background UART receiver, ASCII/binary parser, command API |
| C# client | Windows / Linux / macOS | .NET 8, System.IO.Ports | Async receive task, sensor events, command API |

## Repository layout

```text
StrobeSync-Hybrid-PWM/
├── firmware/
│   ├── stm32/
│   │   ├── Core/Src/main.c
│   │   └── README.md
│   └── arduino/
│       ├── StrobeSyncAVR/StrobeSyncAVR.ino
│       └── README.md
├── clients/
│   ├── python/
│   │   ├── strobesync/client.py
│   │   ├── strobesync/__init__.py
│   │   ├── tests/test_protocol.py
│   │   └── pyproject.toml
│   └── csharp/
│       └── StrobeSyncController/
│           ├── StrobeSyncClient.cs
│           └── StrobeSyncController.csproj
├── examples/csharp/StrobeSyncConsole/
├── docs/
│   ├── PINOUT.md
│   ├── PROTOCOL.md
│   └── VALIDATION.md
├── .github/workflows/clients.yml
├── README.tr.md
└── LICENSE
```

## Quick start

### 1. Choose and flash firmware

- **Arduino:** Open `firmware/arduino/StrobeSyncAVR/StrobeSyncAVR.ino`
  in Arduino IDE, select the matching AVR board/port and upload. See
  [Arduino setup](firmware/arduino/README.md).
- **STM32:** Create a matching CMSIS-enabled project with startup files,
  device headers and linker script, then use
  `firmware/stm32/Core/Src/main.c`. See [STM32 setup](firmware/stm32/README.md).
  The STM32 directory intentionally is **not** a standalone flashable project.

### 2. Install and use the Python client

```bash
python -m pip install -e ./clients/python
```

```python
from strobesync import StrobeSyncClient

client = StrobeSyncClient(
    port="COM3",  # or /dev/ttyUSB0, /dev/ttyACM0, /dev/cu.usbserial-...
    on_sensor_data=lambda sid, value, mode: print(mode, sid, value),
)

try:
    client.connect(reset_delay_sec=2.0)  # use 0 for non-resetting STM32 adapters
    client.set_pwm_channel(channel=0, mode=1, level=75)
    client.configure_sensor(0, StrobeSyncClient.STREAM_BINARY_CONTINUOUS)
    client.trigger_realtime_sync(delay_ms=500)
    input("Press Enter to stop... ")
finally:
    client.close()
```

### 3. Or use the C# client

Install the **.NET 8 SDK**, then:

```bash
dotnet build clients/csharp/StrobeSyncController/StrobeSyncController.csproj
dotnet run --project examples/csharp/StrobeSyncConsole/StrobeSyncConsole.csproj -- COM3
```

Replace `COM3` with your actual serial port.

### 4. Run host-side tests

```bash
python -m pip install -e ./clients/python
python -m unittest discover -s clients/python/tests -v
```

The included GitHub Actions workflow runs these Python tests and compiles the
C# library and example. **It does not compile/flash the embedded firmware.**

## Device capabilities and wiring

- Six PWM outputs: three software PWM and three timer-driven hardware PWM.
- Six synchronized pulse outputs, toggled by master synchronization state.
- Two warning/status GPIO outputs.
- Two ADC sensors and two digital sensors.
- Configured sample intervals: 100 Hz (sensor 0), 50 Hz (sensor 1), and
  200 Hz (sensors 2 and 3). Actual cadence depends on runtime scheduling.
- UART at **115200 baud, 8N1**.
- Telemetry: readable `SENS:<id>:<value>` lines or 5-byte `0xAA` binary frames
  with a simple XOR checksum.
- Sync trigger delays of 0–17,575 ms, encoded in base 26.

The warning-pin mapping differs by platform: **A4/A5 on Arduino** versus
**PC13/PC14 on STM32**. The C# and Python API uses logical warning indices.
For all GPIO assignments and electrical cautions, see [pin maps](docs/PINOUT.md).
For exact framing and binary packet details, see the [protocol](docs/PROTOCOL.md).

## Important limitations

- Continuous sensor modes may drop/coalesce samples; this is latest-value
  streaming, **not** a lossless acquisition logger.
- Snapshot commands return the most recent cached reading, not guaranteed
  to start a new ADC conversion.
- Binary checksum is XOR, **not CRC**.
- UART output buffers can fill and frames may be truncated under load.
- ISR jitter, PWM frequency, phase accuracy, and maximum throughput are
  **not benchmarked**. Review [validation notes](docs/VALIDATION.md).
- Logic-level pins must never directly power a high-current strobe/light;
  add an external driver and electrical protection.

## License

Proposed distribution license: **MIT License** — see [LICENSE](LICENSE).
The license includes the project sources, not a relicensing of third-party
CMSIS, Arduino cores or other dependencies. Confirm the copyright holder
and your right to distribute the source before public publication.

[Türkçe README](README.tr.md)
