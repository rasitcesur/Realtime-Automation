# Validation and limitations

The repository was assembled from user-supplied source code. The three attached
source files (STM32 C, Arduino sketch, and C#) were copied without functional
changes. The Python client was transcribed from the supplied pasted snippet.
Unit tests here exercise **host command encoding, ASCII parsing and mixed ASCII/binary receive parsing only**.
They are not proof that the firmware compiles or meets timing requirements.

## Current verification scope

1. Check Python syntax (`python -m compileall -q clients/python`).
2. Run host encoding/ASCII tests (`python -m unittest discover -s clients/python/tests -v`)
   with `PYTHONPATH=clients/python`, after installing `pyserial`.
3. Build C# clients using the `net8.0` SDK and NuGet access.
4. Independently compile firmware in the relevant CMSIS / AVR project.
5. Verify synchronized outputs and PWM duty/frequency/phase with an oscilloscope.
6. Verify ADC calibration, quantization, noise, and actual sample interval.
7. Stress test serial reception/transmission under full sensor streaming load.

## Engineering concerns to investigate before production

- **AVR timer interactions:** Timer0/Timer2 are controlled through `analogWrite`,
  Timer1 uses an ISR scheduler, and USART0 is configured directly. Avoid
  conflicting Arduino timing/serial libraries. Confirm the resulting hardware
  PWM carrier frequencies and clock assumptions on the specific board.
- **STM32 clock and board setup:** Requires a working 8 MHz external crystal,
  correct startup files and interrupt vectors; HSE/PLL waits currently have
  no timeouts.
- **Concurrent shared state:** Critical sections and data handoff between
  interrupt and main context should be audited on each architecture.
- **Frame integrity:** The outgoing TX buffer is byte-queued, so a full buffer
  can truncate ASCII or 5-byte binary frames. No ACK/retransmission is defined.
- **Host robustness:** Python's current ASCII parser does not range-check
  sensor IDs; callback exceptions are caught by the receive loop and logged.
  C#'s receive ring does not explicitly guard against overrun.
- **Timing:** The source code's timing comments are implementation intentions,
  not measured worst-case jitter, latency, determinism or throughput.
- **Safety:** GPIO output is not a safe power driver. Require suitable
  external driver, thermal/current protection, and a system-level fail-safe.

All safety- or synchronization-critical applications require independent
hardware validation before use.
