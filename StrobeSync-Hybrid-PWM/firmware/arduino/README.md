# AVR / Arduino firmware

**Target:** Arduino Uno / classic Nano using ATmega328P at 16 MHz.

1. Install the Arduino IDE and the Arduino AVR Boards package.
2. Open `StrobeSyncAVR/StrobeSyncAVR.ino` and select **Arduino Uno** or
   **Arduino Nano (ATmega328P)** matching the target hardware.
3. Select the connected port and **Verify** / **Upload**.
4. The firmware configures USART0 directly at **115200 8N1**; host-side
   control is possible from Python or C# through the USB serial port.

**Timer conflicts:** This sketch modifies Timer1 and USART0 registers directly,
uses Timer0/Timer2 PWM, and must not be combined indiscriminately with timing,
UART, Servo, tone, or other timer-dependent libraries. D0/D1 are the UART pins.

For the exact pin mapping see `../../docs/PINOUT.md`; for the wire protocol
see `../../docs/PROTOCOL.md`.

This firmware has not been compiled or tested on a physical target as part of
this packaging operation.
