# STM32F103 CMSIS-only firmware

**Target:** STM32F103x8 / STM32F103xB (ARM Cortex-M3), 8 MHz HSE crystal,
72 MHz system clock. This directory contains the application's source only,
**not** a complete flashable STM32 project.

## Import into an STM32 toolchain

1. Create a bare-metal / CMSIS STM32F103x8 or STM32F103xB project in your
   preferred IDE or make-based toolchain (for example, STM32CubeIDE).
2. Provide the corresponding STM32F1 CMSIS device pack / `stm32f1xx.h`,
   startup assembly, matching device definition (`STM32F103xB` for the
   applicable device), linker script, and suitable system files.
3. Replace the generated `main.c` entry point with `Core/Src/main.c` from
   this directory. There must be **only one** `main` entry point.
4. Build with the correct MCU, linker script, startup vectors, and clock
   configuration. Ensure the selected board actually exposes the listed pins.
5. Flash and check UART1 on **PA9 (TX) / PA10 (RX), 115200 8N1**.

This implementation uses CMSIS register access, not STM32 HAL or Arduino.
The PC13/PC14 warning outputs and PB3/PB4 pins require board-specific
checks; the firmware disables JTAG while retaining SWD for PB3/PB4 use.
Do **not** connect 5 V UART directly to 3.3 V STM32 RX.

See `../../docs/PINOUT.md` and `../../docs/PROTOCOL.md`.

This folder cannot be built as-is without a CMSIS-enabled project and was not
compiled or hardware-verified during packaging.
