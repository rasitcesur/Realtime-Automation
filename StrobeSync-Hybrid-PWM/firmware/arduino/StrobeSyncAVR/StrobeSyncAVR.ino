#include <Arduino.h>
#include <avr/io.h>
#include <avr/interrupt.h>

// ============================================================================
// SYSTEM CONFIGURATION & CONSTANTS
// ============================================================================
const uint8_t PWM_CHANNEL_COUNT    = 6;
const uint8_t TIMELINE_EVENT_COUNT = PWM_CHANNEL_COUNT * 2;
const uint8_t PULSE_OUTPUT_COUNT   = 6;
const uint8_t WARNING_PIN_COUNT    = 2;
const uint8_t SENSOR_COUNT         = 4;

const unsigned long DEFAULT_PWM_PERIOD_US   = 1000;    // 1 ms (1 kHz) Soft-PWM base period
const unsigned long SYNC_TOGGLE_INTERVAL_US = 1000000; // 1 second (in us) for master sync toggle
const unsigned long INACTIVE_EVENT_MARKER   = 999999;  // Marker for disabled timeline slots

// UART Ring Buffer Configuration (Power of 2 for single-cycle bitwise AND wrapping)
const uint16_t UART_TX_BUFFER_SIZE = 256;
const uint8_t  UART_TX_BUFFER_MASK = UART_TX_BUFFER_SIZE - 1;
const uint8_t  UART_RX_BUFFER_SIZE = 64;
const uint8_t  UART_RX_BUFFER_MASK = UART_RX_BUFFER_SIZE - 1;
const uint8_t  CMD_FRAME_MAX_LEN   = 8;
const uint8_t  BINARY_SYNC_HEADER  = 0xAA;

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * Defines a PWM channel with pre-cached port registers for single-cycle ISR IO
 */
struct PwmChannel {
  uint8_t pin;
  bool isHardwarePwm;               // true: Timer0/Timer2 HW PWM, false: Timer1 ISR Soft-PWM
  volatile uint8_t mode;            // 0: Continuous, 1: Synchronized with masterSyncState
  volatile uint8_t level;           // Duty cycle percentage (0 - 100)
  volatile unsigned long dutyDurationUs;
  volatile unsigned long offDurationUs;
  volatile uint8_t *portRegister;   // Cached MCU PORTx register for fast output
  uint8_t bitMask;                  // Cached bitmask for pin
};

/**
 * Defines an event node in the ISR-driven Software PWM timeline
 */
struct TimelineEvent {
  volatile unsigned long startDelayUs;    // Time remaining until event starts (0 = active)
  volatile unsigned long remainingTimeUs; // Time remaining until event finishes
  volatile unsigned long totalDurationUs; // Total pre-calculated duration of this state
};

/**
 * Defines a fully interrupt-driven sensor task (Digital via PINx, Analog via ADC_vect)
 */
struct SensorChannel {
  uint8_t pin;
  bool isAnalog;                          // true: Asynchronous ADC ISR, false: Direct PINx read in Timer1
  uint8_t adcChannel;                     // Hardware ADC multiplexer index (0-7 for A0-A7)
  volatile uint8_t *inputRegister;        // Cached MCU PINx register for single-cycle digital read
  uint8_t bitMask;                        // Cached bitmask for digital read
  unsigned long sampleIntervalUs;         // Sampling period in microseconds
  volatile unsigned long elapsedUs;       // Time accumulator updated inside Timer1 ISR
  volatile bool adcPending;               // Queued flag for the ADC state machine
  volatile bool dataReady;                // Set when a fresh reading is completed by ISR
  volatile uint16_t lastValue;            // Latest 10-bit ADC or 1-bit digital reading
  volatile uint8_t streamMode;            // 0: Off, 1: ASCII ("SENS:i:val\n"), 3: Binary Frame (5 bytes)
};

// ============================================================================
// HARDWARE & TASK TABLE DEFINITIONS
// ============================================================================

// Pins 3, 5, 6 use Hardware PWM (Timer2 & Timer0). Pins 2, 4, 7 use Timer1 ISR Soft-PWM.
PwmChannel pwmChannels[PWM_CHANNEL_COUNT] = {
  {2, false, 0, 80, 0, 0, nullptr, 0}, // Channel 0: Soft-PWM (Timer1 ISR)
  {3, true,  0, 80, 0, 0, nullptr, 0}, // Channel 1: Hardware PWM (Timer2)
  {4, false, 0, 80, 0, 0, nullptr, 0}, // Channel 2: Soft-PWM (Timer1 ISR)
  {5, true,  0, 80, 0, 0, nullptr, 0}, // Channel 3: Hardware PWM (Timer0)
  {6, true,  0, 80, 0, 0, nullptr, 0}, // Channel 4: Hardware PWM (Timer0)
  {7, false, 0, 80, 0, 0, nullptr, 0}  // Channel 5: Soft-PWM (Timer1 ISR)
};

TimelineEvent eventTimeline[TIMELINE_EVENT_COUNT];

const uint8_t pulseOutputPins[PULSE_OUTPUT_COUNT] = {8, 9, 10, 11, 12, 13};
const uint8_t warningPins[WARNING_PIN_COUNT]      = {A4, A5};

// Sensor Task Table: Analog channels driven by ADC_vect, Digital channels by TIMER1_COMPA_vect
SensorChannel sensorTable[SENSOR_COUNT] = {
  {A0, true,  0, nullptr, 0, 10000, 0, false, false, 0, 0}, // Analog Sensor 0 (ADC0): 10 ms (100 Hz)
  {A1, true,  1, nullptr, 0, 20000, 0, false, false, 0, 0}, // Analog Sensor 1 (ADC1): 20 ms (50 Hz)
  {A2, false, 2, nullptr, 0, 5000,  0, false, false, 0, 0}, // Digital Sensor 2 (PINC2): 5 ms (200 Hz)
  {A3, false, 3, nullptr, 0, 5000,  0, false, false, 0, 0}  // Digital Sensor 3 (PINC3): 5 ms (200 Hz)
};

// ============================================================================
// REAL-TIME SYNCHRONIZATION, ADC & UART STATE VARIABLES (ISR-SHARED)
// ============================================================================
volatile long syncCountdownUs          = -1;
volatile unsigned long syncToggleAccum = 0;
volatile bool syncTriggerActive        = false;
volatile uint8_t masterSyncState       = 0;
volatile bool masterStateChangedFlag   = false;

volatile unsigned long currentIsrStepUs = 1000;

// Asynchronous ADC State Machine Variables
volatile int8_t currentAdcSensorIdx = -1;
volatile bool adcBusy               = false;

// Lock-Free UART Ring Buffers
volatile uint8_t uartTxBuffer[UART_TX_BUFFER_SIZE];
volatile uint8_t uartTxHead = 0;
volatile uint8_t uartTxTail = 0;

volatile uint8_t uartRxBuffer[UART_RX_BUFFER_SIZE];
volatile uint8_t uartRxHead = 0;
volatile uint8_t uartRxTail = 0;

// Non-Blocking Command Frame Parser State
uint8_t cmdFrameBuffer[CMD_FRAME_MAX_LEN];
uint8_t cmdFrameIndex = 0;

// ============================================================================
// BARE-METAL NON-BLOCKING UART ENGINE (USART0)
// ============================================================================

/**
 * Initializes USART0 at 115200 baud (8N1) using Double Speed Mode (U2X0)
 * Completely replaces Arduino's blocking HardwareSerial library
 */
void setupUartIsr(uint32_t baudRate) {
  noInterrupts();

  // Calculate UBRR0 for Double Speed mode (U2X0 = 1)
  uint16_t ubrrValue = (uint16_t)((F_CPU / (8UL * baudRate)) - 1UL);
  UBRR0H = (uint8_t)(ubrrValue >> 8);
  UBRR0L = (uint8_t)(ubrrValue & 0xFF);

  UCSR0A = (1 << U2X0);                                  // Enable 2x speed for minimal baud error at 115200
  UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);  // Enable RX, TX, and RX Complete Interrupt
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);                // 8-bit data, 1 stop bit, no parity (8N1)

  interrupts();
}

/**
 * USART Receive Complete ISR
 * Captures incoming byte in < 1 us and pushes to lock-free RX ring buffer
 */
ISR(USART_RX_vect) {
  uint8_t receivedByte = UDR0;
  uint8_t nextHead = (uartRxHead + 1) & UART_RX_BUFFER_MASK;
  if (nextHead != uartRxTail) {
    uartRxBuffer[uartRxHead] = receivedByte;
    uartRxHead = nextHead;
  }
}

/**
 * USART Data Register Empty ISR
 * Feeds the next queued byte from the TX ring buffer into hardware UDR0
 */
ISR(USART_UDRE_vect) {
  if (uartTxHead == uartTxTail) {
    // Buffer empty: disable UDRE interrupt until new data is queued
    UCSR0B &= ~(1 << UDRIE0);
  } else {
    UDR0 = uartTxBuffer[uartTxTail];
    uartTxTail = (uartTxTail + 1) & UART_TX_BUFFER_MASK;
  }
}

/**
 * Enqueues a single byte into the TX ring buffer without blocking.
 * If the buffer is full, drops the byte immediately to guarantee real-time loop determinism.
 */
inline bool uartWriteByte(uint8_t data) {
  uint8_t nextHead = (uartTxHead + 1) & UART_TX_BUFFER_MASK;
  if (nextHead == uartTxTail) {
    return false; // Buffer full: drop byte rather than blocking CPU
  }
  uartTxBuffer[uartTxHead] = data;
  uartTxHead = nextHead;

  // Enable Data Register Empty Interrupt to start hardware transmission
  UCSR0B |= (1 << UDRIE0);
  return true;
}

/**
 * Enqueues a raw byte array into the TX ring buffer
 */
void uartWriteBytes(const uint8_t *data, uint8_t length) {
  for (uint8_t i = 0; i < length; i++) {
    if (!uartWriteByte(data[i])) break;
  }
}

/**
 * Fast non-blocking ASCII telemetry formatter ("SENS:<id>:<val>\n")
 * Uses fast digit extraction without slow printf/itoa library calls
 */
void sendSensorAsciiFrame(uint8_t sensorIdx, uint16_t value) {
  uint8_t frame[16];
  frame[0] = 'S';
  frame[1] = 'E';
  frame[2] = 'N';
  frame[3] = 'S';
  frame[4] = ':';
  frame[5] = '0' + sensorIdx;
  frame[6] = ':';

  uint8_t pos = 7;
  if (value == 0) {
    frame[pos++] = '0';
  } else {
    uint8_t tempDigits[5];
    uint8_t digitCount = 0;
    while (value > 0) {
      tempDigits[digitCount++] = '0' + (value % 10);
      value /= 10;
    }
    while (digitCount > 0) {
      frame[pos++] = tempDigits[--digitCount];
    }
  }
  frame[pos++] = '\n';

  uartWriteBytes(frame, pos);
}

/**
 * Ultra-fast 5-byte binary telemetry frame:
 * [0xAA (Header)] [SensorIdx] [ValHigh] [ValLow] [XOR Checksum]
 */
inline void sendSensorBinaryFrame(uint8_t sensorIdx, uint16_t value) {
  uint8_t highByte = (uint8_t)(value >> 8);
  uint8_t lowByte  = (uint8_t)(value & 0xFF);
  uint8_t checksum = BINARY_SYNC_HEADER ^ sensorIdx ^ highByte ^ lowByte;

  uint8_t packet[5] = {BINARY_SYNC_HEADER, sensorIdx, highByte, lowByte, checksum};
  uartWriteBytes(packet, 5);
}

// ============================================================================
// FAST IO & ASYNCHRONOUS ADC ENGINE
// ============================================================================

/**
 * Sets a pin state in a single CPU cycle using cached port registers
 */
inline void fastDigitalWrite(volatile uint8_t *portReg, uint8_t bitMask, bool state) {
  if (state) {
    *portReg |= bitMask;
  } else {
    *portReg &= ~bitMask;
  }
}

/**
 * Starts the next pending analog conversion if the ADC hardware is idle
 */
inline void triggerNextAdcConversion() {
  if (adcBusy) return;

  uint8_t startIdx = (currentAdcSensorIdx >= 0) ? ((currentAdcSensorIdx + 1) % SENSOR_COUNT) : 0;

  for (uint8_t count = 0; count < SENSOR_COUNT; count++) {
    uint8_t idx = (startIdx + count) % SENSOR_COUNT;
    if (sensorTable[idx].isAnalog && sensorTable[idx].adcPending) {
      sensorTable[idx].adcPending = false;
      currentAdcSensorIdx = idx;
      adcBusy = true;

      ADMUX = (1 << REFS0) | (sensorTable[idx].adcChannel & 0x07);
      ADCSRA |= (1 << ADSC);
      break;
    }
  }
}

/**
 * ADC Conversion Complete ISR
 */
ISR(ADC_vect) {
  uint16_t result = ADC;

  if (currentAdcSensorIdx >= 0 && currentAdcSensorIdx < SENSOR_COUNT) {
    sensorTable[currentAdcSensorIdx].lastValue = result;
    sensorTable[currentAdcSensorIdx].dataReady = true;
  }

  adcBusy = false;
  triggerNextAdcConversion();
}

/**
 * Initializes the hardware ADC for interrupt-driven single conversions
 */
void setupAdcIsr() {
  noInterrupts();
  ADMUX  = (1 << REFS0);
  ADCSRA = (1 << ADEN) | (1 << ADIE) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
  ADCSRB = 0;

  uint8_t didrMask = 0;
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    if (sensorTable[i].isAnalog && sensorTable[i].adcChannel < 6) {
      didrMask |= (1 << sensorTable[i].adcChannel);
    }
  }
  DIDR0 = didrMask;
  interrupts();
}

// ============================================================================
// HARDWARE PWM & TIMELINE MANAGEMENT
// ============================================================================

/**
 * Updates hardware PWM output via Timer0/Timer2
 */
void applyHardwarePwm(uint8_t channelIdx) {
  PwmChannel &ch = pwmChannels[channelIdx];
  if (!ch.isHardwarePwm) return;

  bool isOutputEnabled = (ch.mode == 0) || (ch.mode == 1 && masterSyncState == 1);
  if (isOutputEnabled && ch.level > 0) {
    uint8_t pwmValue = map(constrain(ch.level, 0, 100), 0, 100, 0, 255);
    analogWrite(ch.pin, pwmValue);
  } else {
    analogWrite(ch.pin, 0);
    digitalWrite(ch.pin, LOW);
  }
}

/**
 * Updates PWM channel configuration safely by pausing interrupts during timeline rebuild
 */
void updatePwmChannel(uint8_t channelIdx, uint8_t mode, uint8_t level) {
  if (channelIdx >= PWM_CHANNEL_COUNT) return;

  uint8_t constrainedLevel = constrain(level, 0, 100);
  float dutyRatio = (float)constrainedLevel / 100.0f;
  unsigned long dCycle = (unsigned long)(DEFAULT_PWM_PERIOD_US * dutyRatio);
  unsigned long oCycle = (constrainedLevel > 0) ? (DEFAULT_PWM_PERIOD_US - dCycle) : 0;

  noInterrupts();

  PwmChannel &targetCh = pwmChannels[channelIdx];
  targetCh.mode           = mode;
  targetCh.level          = constrainedLevel;
  targetCh.dutyDurationUs = dCycle;
  targetCh.offDurationUs  = oCycle;

  for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
    uint8_t dutyIdx = i * 2;
    uint8_t offIdx  = dutyIdx + 1;

    if (pwmChannels[i].isHardwarePwm || pwmChannels[i].level == 0) {
      eventTimeline[dutyIdx] = {INACTIVE_EVENT_MARKER, INACTIVE_EVENT_MARKER, INACTIVE_EVENT_MARKER};
      eventTimeline[offIdx]  = {INACTIVE_EVENT_MARKER, INACTIVE_EVENT_MARKER, INACTIVE_EVENT_MARKER};
      if (!pwmChannels[i].isHardwarePwm) {
        fastDigitalWrite(pwmChannels[i].portRegister, pwmChannels[i].bitMask, LOW);
      }
    } else {
      unsigned long chDuty = pwmChannels[i].dutyDurationUs;
      unsigned long chOff  = pwmChannels[i].offDurationUs;

      eventTimeline[dutyIdx].startDelayUs    = 0;
      eventTimeline[dutyIdx].remainingTimeUs = chDuty;
      eventTimeline[dutyIdx].totalDurationUs = chDuty;

      eventTimeline[offIdx].startDelayUs    = chDuty;
      eventTimeline[offIdx].remainingTimeUs = chDuty + chOff;
      eventTimeline[offIdx].totalDurationUs = chOff;

      fastDigitalWrite(pwmChannels[i].portRegister, pwmChannels[i].bitMask, LOW);
    }
  }

  interrupts();

  if (targetCh.isHardwarePwm) {
    applyHardwarePwm(channelIdx);
  }
}

// ============================================================================
// TIMER1 INTERRUPT SERVICE ROUTINE (REAL-TIME ENGINE)
// ============================================================================

/**
 * Timer1 Compare Match A ISR
 */
ISR(TIMER1_COMPA_vect) {
  unsigned long stepExecutedUs = currentIsrStepUs;
  unsigned long nextTimeSliceUs = INACTIVE_EVENT_MARKER;

  // 1. Advance Soft-PWM Timeline and toggle pins with zero jitter
  for (uint8_t i = 0; i < TIMELINE_EVENT_COUNT; i++) {
    uint8_t channelIdx = i >> 1;
    if (pwmChannels[channelIdx].isHardwarePwm) continue;

    TimelineEvent &ev = eventTimeline[i];
    if (ev.remainingTimeUs >= INACTIVE_EVENT_MARKER) continue;

    if (ev.startDelayUs == 0 && ev.remainingTimeUs != 0) {
      ev.remainingTimeUs = (ev.remainingTimeUs > stepExecutedUs) ? (ev.remainingTimeUs - stepExecutedUs) : 0;

      if ((i & 1) == 0) {
        bool outputActive = (pwmChannels[channelIdx].mode == 0) ||
                            (pwmChannels[channelIdx].mode == 1 && masterSyncState == 1);
        fastDigitalWrite(pwmChannels[channelIdx].portRegister, pwmChannels[channelIdx].bitMask, outputActive);

        if (ev.remainingTimeUs == 0) {
          ev.startDelayUs    = eventTimeline[i + 1].totalDurationUs;
          ev.remainingTimeUs = ev.totalDurationUs + ev.startDelayUs;
        }
      } else {
        if (ev.remainingTimeUs == 0) {
          ev.startDelayUs    = eventTimeline[i - 1].totalDurationUs;
          ev.remainingTimeUs = ev.totalDurationUs + ev.startDelayUs;
        }
        fastDigitalWrite(pwmChannels[channelIdx].portRegister, pwmChannels[channelIdx].bitMask, LOW);
      }
    } else if (ev.startDelayUs > 0) {
      ev.startDelayUs    = (ev.startDelayUs > stepExecutedUs) ? (ev.startDelayUs - stepExecutedUs) : 0;
      ev.remainingTimeUs = (ev.remainingTimeUs > stepExecutedUs) ? (ev.remainingTimeUs - stepExecutedUs) : 0;
    }

    if (ev.startDelayUs == 0 && ev.remainingTimeUs > 0 && ev.remainingTimeUs < nextTimeSliceUs) {
      nextTimeSliceUs = ev.remainingTimeUs;
    }
  }

  if (nextTimeSliceUs == INACTIVE_EVENT_MARKER || nextTimeSliceUs == 0) {
    nextTimeSliceUs = 500;
  }
  if (nextTimeSliceUs < 25) {
    nextTimeSliceUs = 25;
  }

  // 2. Update Real-Time Synchronization Countdown
  if (syncCountdownUs >= 0) {
    syncCountdownUs -= (long)stepExecutedUs;
    if (syncCountdownUs <= 0) {
      syncTriggerActive = true;
      syncToggleAccum   = SYNC_TOGGLE_INTERVAL_US;
      syncCountdownUs   = -1;
    }
  }

  // 3. Handle Master Synchronization State Toggling (1 Hz / 1000 ms)
  if (syncTriggerActive) {
    syncToggleAccum += stepExecutedUs;
    if (syncToggleAccum >= SYNC_TOGGLE_INTERVAL_US) {
      syncToggleAccum -= SYNC_TOGGLE_INTERVAL_US;
      masterSyncState = (masterSyncState + 1) & 1;
      masterStateChangedFlag = true;
    }
  }

  // 4. Update Sensor Timers: Read Digital immediately via PINx, Queue Analog for ADC ISR
  bool newAdcQueued = false;
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    SensorChannel &sensor = sensorTable[i];
    sensor.elapsedUs += stepExecutedUs;

    if (sensor.elapsedUs >= sensor.sampleIntervalUs) {
      sensor.elapsedUs -= sensor.sampleIntervalUs;

      if (sensor.isAnalog) {
        sensor.adcPending = true;
        newAdcQueued = true;
      } else {
        sensor.lastValue = ((*sensor.inputRegister & sensor.bitMask) != 0) ? 1 : 0;
        sensor.dataReady = true;
      }
    }
  }

  if (newAdcQueued && !adcBusy) {
    triggerNextAdcConversion();
  }

  // 5. Program Timer1 OCR1A for the next time slice (Prescaler 8 -> 1 us = 2 ticks at 16 MHz)
  currentIsrStepUs = nextTimeSliceUs;
  unsigned long timerTicks = (nextTimeSliceUs << 1) - 1;
  if (timerTicks > 65535) timerTicks = 65535;
  OCR1A = (uint16_t)timerTicks;
}

// ============================================================================
// NON-BLOCKING TELEMETRY & COMMAND PARSER STATE MACHINE
// ============================================================================

/**
 * Configures Timer1 in CTC Mode with Prescaler 8 (0.5 us resolution at 16 MHz)
 */
void setupTimer1Isr() {
  noInterrupts();
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1  = 0;

  currentIsrStepUs = 100;
  OCR1A = 199;

  TCCR1B |= (1 << WGM12);
  TCCR1B |= (1 << CS11);
  TIMSK1 |= (1 << OCIE1A);
  interrupts();
}

/**
 * Atomically reads the latest sensor value and queues telemetry into the UART TX ring buffer
 */
void serviceSensorTelemetry() {
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    if (sensorTable[i].dataReady) {
      noInterrupts();
      sensorTable[i].dataReady = false;
      uint16_t capturedValue = sensorTable[i].lastValue;
      uint8_t mode = sensorTable[i].streamMode;
      interrupts();

      if (mode == 1) {
        sendSensorAsciiFrame(i, capturedValue);
      } else if (mode == 3) {
        sendSensorBinaryFrame(i, capturedValue);
      }
    }
  }
}

/**
 * Services master synchronization hardware updates flagged by the ISR
 */
void serviceMasterSyncUpdates() {
  if (masterStateChangedFlag) {
    masterStateChangedFlag = false;
    uint8_t state = masterSyncState;

    for (uint8_t i = 0; i < PULSE_OUTPUT_COUNT; i++) {
      digitalWrite(pulseOutputPins[i], state);
    }

    for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
      if (pwmChannels[i].isHardwarePwm && pwmChannels[i].mode == 1) {
        applyHardwarePwm(i);
      }
    }
  }
}

/**
 * Executes a validated command frame without blocking
 */
void executeCommandFrame(const uint8_t *frame, uint8_t length) {
  if (length < 3) return;

  int baseOffset = frame[0];
  int cmdTarget  = frame[1] - baseOffset;
  int param1     = frame[2] - baseOffset;

  if (cmdTarget < 0 || param1 < 0) return;

  // 1. Configure PWM Channels (0 to 5)
  if (cmdTarget < PWM_CHANNEL_COUNT && length >= 5) {
    int highNibble = frame[3] - baseOffset;
    int lowNibble  = frame[4] - baseOffset;
    if (highNibble >= 0 && lowNibble >= 0) {
      uint8_t level = (uint8_t)(highNibble * 16 + lowNibble);
      updatePwmChannel((uint8_t)cmdTarget, (uint8_t)param1, level);
    }
  }
  // 2. Sensor Control & Query Commands (6 to 10)
  // param1 -> 0: Stop, 1: Stream ASCII, 2: Single Read (ASCII), 3: Stream Binary (5-byte), 4: Single Read (Binary)
  else if (cmdTarget < 11) {
    uint8_t sensorIdx = cmdTarget - 6;
    if (sensorIdx < SENSOR_COUNT) {
      if (param1 == 2 || param1 == 4) {
        noInterrupts();
        uint16_t snapshotVal = sensorTable[sensorIdx].lastValue;
        interrupts();

        if (param1 == 2) sendSensorAsciiFrame(sensorIdx, snapshotVal);
        else             sendSensorBinaryFrame(sensorIdx, snapshotVal);
      } else {
        sensorTable[sensorIdx].streamMode = (uint8_t)param1;
      }
    }
  }
  // 3. Warning Pins Control (11 to 12)
  else if (cmdTarget < 13) {
    uint8_t warnIdx = cmdTarget - 11;
    if (warnIdx < WARNING_PIN_COUNT) {
      digitalWrite(warningPins[warnIdx], (param1 > 0) ? HIGH : LOW);
    }
  }
  // 4. Real-Time Synchronization Trigger (Command ID 25)
  else if (cmdTarget == 25 && length >= 5) {
    int param2 = frame[3] - baseOffset;
    int param3 = frame[4] - baseOffset;
    if (param2 >= 0 && param3 >= 0) {
      unsigned long delayMs = (unsigned long)param3 + (unsigned long)param2 * 26UL + (unsigned long)param1 * 26UL * 26UL;
      noInterrupts();
      syncCountdownUs   = (long)(delayMs * 1000UL);
      syncTriggerActive = false;
      masterSyncState   = 0;
      interrupts();
    }
  }
}

/**
 * Drains available bytes from the lock-free RX ring buffer and feeds the frame parser state machine.
 * Returns immediately if no bytes are available (0 us wait time).
 */
void pollUartRxStateMachine() {
  while (uartRxTail != uartRxHead) {
    uint8_t incomingByte = uartRxBuffer[uartRxTail];
    uartRxTail = (uartRxTail + 1) & UART_RX_BUFFER_MASK;

    if (incomingByte == '\n' || incomingByte == '\r') {
      if (cmdFrameIndex > 0) {
        executeCommandFrame(cmdFrameBuffer, cmdFrameIndex);
        cmdFrameIndex = 0;
      }
    } else {
      if (cmdFrameIndex < CMD_FRAME_MAX_LEN) {
        cmdFrameBuffer[cmdFrameIndex++] = incomingByte;
      } else {
        // Frame overflow protection: reset state machine
        cmdFrameIndex = 0;
      }
    }
  }
}

// ============================================================================
// ARDUINO SETUP & LOOP
// ============================================================================

void setup() {
  // Initialize Bare-Metal Interrupt-Driven UART at 115200 baud
  setupUartIsr(115200);

  // Initialize PWM pins and cache output port registers/bitmasks
  for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
    uint8_t pin = pwmChannels[i].pin;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    pwmChannels[i].portRegister = portOutputRegister(digitalPinToPort(pin));
    pwmChannels[i].bitMask      = digitalPinToBitMask(pin);
  }

  for (uint8_t i = 0; i < PULSE_OUTPUT_COUNT; i++) {
    pinMode(pulseOutputPins[i], OUTPUT);
    digitalWrite(pulseOutputPins[i], LOW);
  }

  for (uint8_t i = 0; i < WARNING_PIN_COUNT; i++) {
    pinMode(warningPins[i], OUTPUT);
    digitalWrite(warningPins[i], LOW);
  }

  // Initialize sensor pins, cache PINx input registers, and map ADC multiplexer channels
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    uint8_t pin = sensorTable[i].pin;
    pinMode(pin, INPUT);
    sensorTable[i].inputRegister = portInputRegister(digitalPinToPort(pin));
    sensorTable[i].bitMask       = digitalPinToBitMask(pin);
    if (sensorTable[i].isAnalog) {
      sensorTable[i].adcChannel = (pin >= A0) ? (pin - A0) : pin;
    }
  }

  // Initialize default PWM states (80% duty cycle, continuous mode)
  for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
    updatePwmChannel(i, 0, 80);
  }

  // Start Asynchronous ADC Engine & Hardware Timer1 ISR Engine
  setupAdcIsr();
  setupTimer1Isr();
}

void loop() {
  // 100% Bare-Metal Non-Blocking Event Loop:
  // - UART RX/TX runs via USART_RX_vect & USART_UDRE_vect
  // - Analog sampling runs via ADC_vect
  // - Soft-PWM & Digital sampling run via TIMER1_COMPA_vect
  pollUartRxStateMachine();
  serviceMasterSyncUpdates();
  serviceSensorTelemetry();
}