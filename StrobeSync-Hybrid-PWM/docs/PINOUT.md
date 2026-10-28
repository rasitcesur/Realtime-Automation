# Pin maps and electrical connections

The **logical channel indices** are the same across both firmware targets;
the **physical pins are different**.

## STM32F103x8 / STM32F103xB

| Function | Channel | STM32 pin | Implementation |
|---|---:|---|---|
| PWM | 0 | PB12 | Software PWM (TIM2 ISR) |
| PWM | 1 | PA8 | Hardware TIM1 CH1 |
| PWM | 2 | PB13 | Software PWM (TIM2 ISR) |
| PWM | 3 | PB0 | Hardware TIM3 CH3 |
| PWM | 4 | PB1 | Hardware TIM3 CH4 |
| PWM | 5 | PB14 | Software PWM (TIM2 ISR) |
| Pulse | 0–5 | PB3, PB4, PB5, PB6, PB7, PB8 | Synchronized outputs |
| Warning | 0–1 | PC13, PC14 | GPIO outputs |
| Sensor | 0 | PA0 | ADC1 IN0, analog 12-bit, configured 100 Hz |
| Sensor | 1 | PA1 | ADC1 IN1, analog 12-bit, configured 50 Hz |
| Sensor | 2 | PA2 | Digital GPIO, configured 200 Hz |
| Sensor | 3 | PA3 | Digital GPIO, configured 200 Hz |
| UART | TX / RX | PA9 / PA10 | USART1, 115200 8N1 |

HSE is assumed to be 8 MHz. PB3 and PB4 are used as GPIO by disabling JTAG,
retaining SWD. PC14 may also be assigned to the LSE oscillator on some boards.
Check that the actual package and board expose each required pin.

## Arduino Uno / classic Nano (ATmega328P, 16 MHz)

| Function | Channel | Arduino pin | Implementation |
|---|---:|---|---|
| PWM | 0 | D2 | Software PWM (Timer1 ISR) |
| PWM | 1 | D3 | Hardware PWM (Timer2) |
| PWM | 2 | D4 | Software PWM (Timer1 ISR) |
| PWM | 3 | D5 | Hardware PWM (Timer0) |
| PWM | 4 | D6 | Hardware PWM (Timer0) |
| PWM | 5 | D7 | Software PWM (Timer1 ISR) |
| Pulse | 0–5 | D8, D9, D10, D11, D12, D13 | Synchronized outputs |
| Warning | 0–1 | A4, A5 | GPIO outputs (also I2C pins) |
| Sensor | 0 | A0 | Analog 10-bit, configured 100 Hz |
| Sensor | 1 | A1 | Analog 10-bit, configured 50 Hz |
| Sensor | 2 | A2 | Digital GPIO, configured 200 Hz |
| Sensor | 3 | A3 | Digital GPIO, configured 200 Hz |
| UART | TX / RX | D1 / D0 | USART0 via board's USB UART, 115200 8N1 |

## Connection guidelines

- Connect **GND** between the host USB-UART adapter and the device.
- Cross the UART wires: adapter TX → device RX; adapter RX ← device TX.
- STM32F103 GPIO is 3.3 V logic; use compatible UART levels and common ground.
- The MCU GPIO must drive a properly rated LED driver/MOSFET interface, **not**
  an external strobe, motor, or high-current lamp directly.
- Digital sensor inputs in the supplied code are floating inputs. Provide
  external bias resistors or modify the code to configure pulls as appropriate.
- Validate pulse timing and power stages with an oscilloscope before deploying
  to production machinery or camera/lighting synchronization hardware.
