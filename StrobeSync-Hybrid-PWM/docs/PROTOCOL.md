# UART protocol (v0.1 draft)

Both firmware versions implement the same *command/telemetry framing* and the
host clients generate compatible commands. The firmware parser interprets
command bytes relative to the first byte; the supplied clients always use
ASCII `A` (`0x41`) as the base.

## Transport

- UART: **115200 baud, 8 data bits, no parity, 1 stop bit (8N1)**.
- Host commands are newline-terminated (`\n`, `0x0A`). Firmware also
  accepts `\r` as a delimiter.
- Commands have no acknowledgement, retry protocol, or error response.

## Host → MCU commands

The first byte is `B = 0x41`. A logical value `n` is sent as `B + n`.

| Command | Logical target | Frame before LF | Notes |
|---|---:|---|---|
| Set PWM | 0–5 | `[B, B+target, B+mode, B+highNibble, B+lowNibble]` | Duty = `16 * highNibble + lowNibble`, normally 0–100; mode 0 = continuous, 1 = sync-gated |
| Sensor command | 6–9 | `[B, B+6+sensorId, B+streamMode]` | Sensor IDs 0–3; see modes below |
| Set warning output | 11–12 | `[B, B+11+warningId, B+state]` | `state = 0` off, `1` on |
| Trigger sync | 25 | `[B, B+25, B+p1, B+p2, B+p3]` | Delay in milliseconds, base 26, max 17,575 ms |

Sync delay encoding: `delayMs = p1*676 + p2*26 + p3`, with `0 ≤ p1,p2,p3 ≤ 25`.
The firmware starts a countdown and toggles the master sync state at
**1-second intervals** after activation. Consequently, a full high/low
blink cycle is approximately **2 seconds** (0.5 Hz), not 1 Hz.

### Stream modes

| Mode | Meaning | Firmware behavior |
|---:|---|---|
| 0 | Off | Stop continuous telemetry |
| 1 | ASCII continuous | Emit the latest sample when `dataReady` is serviced |
| 2 | ASCII single | Immediately return the **last cached** sample |
| 3 | Binary continuous | Emit the latest sample when `dataReady` is serviced |
| 4 | Binary single | Immediately return the **last cached** sample |

A single-read command is **not** guaranteed to initiate a fresh ADC conversion.
Analog ADC width: **12-bit STM32**, **10-bit Arduino**; digital values are 0/1.

### Command examples

- PWM channel 0, synchronized, duty 75%: `AABEL\n` (ASCII bytes).
- Sensor 0, binary continuous: `AGD\n`.
- Sync trigger after 500 ms: `AZATG\n`.

## MCU → Host telemetry

**ASCII:** `SENS:<sensorId>:<value>\n`, for example `SENS:0:1023\n`.

**Binary:** exactly 5 bytes, **without LF**:

```
byte 0    byte 1     byte 2      byte 3     byte 4
0xAA      sensorId   value MSB   value LSB XOR checksum
```

The XOR checksum is `0xAA ^ sensorId ^ valueMSB ^ valueLSB`.
It is a small integrity check, **not** a CRC or cryptographic protection.
A receiver should reject invalid IDs and checksums and scan for the next
`0xAA` header on corruption. Binary frame payloads may contain `0x0A` or `0xAA`.

## Known constraints

- Ring buffers are finite. Firmware intentionally drops outgoing bytes when
  a TX buffer fills; writes are not frame-atomic and can therefore result in
  truncated/invalid frames at overload. Host clients should tolerate loss.
- The `dataReady` flag stores only the latest sensor sample; slow UART service
  can coalesce successive measurements instead of buffering every sample.
- ISR scheduler granularity, actual PWM frequency, jitter and timer interaction
  require **target-specific measurement**; no hard real-time bound is certified.
- No device discovery, protocol versioning, sequence numbers, or authentication.
- The supplied firmware accepts command offsets relative to the first byte;
  do not use this protocol over untrusted serial links without validation.
