/**
 * @file    main.c
 * @brief   Bare-Metal CMSIS Hybrid-PWM, Dynamic Timeline Scheduler & Async Telemetry Engine
 * @target  STM32F103x8 / STM32F103xB (72 MHz ARM Cortex-M3)
 * @note    Zero Arduino/HAL dependencies. 100% compatible with the Python & C# clients.
 */

#include "stm32f1xx.h"
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// SYSTEM CONFIGURATION & CONSTANTS
// ============================================================================
#define PWM_CHANNEL_COUNT        6U
#define TIMELINE_EVENT_COUNT     (PWM_CHANNEL_COUNT * 2U)
#define PULSE_OUTPUT_COUNT       6U
#define WARNING_PIN_COUNT        2U
#define SENSOR_COUNT             4U

#define DEFAULT_PWM_PERIOD_US    1000UL    // 1 ms (1 kHz) base period for HW & Soft PWM
#define SYNC_TOGGLE_INTERVAL_US  1000000UL // 1 second (in us) for master sync toggle
#define INACTIVE_EVENT_MARKER    999999UL  // Marker for disabled timeline slots

// Lock-Free UART Ring Buffer Sizes (Must be power of 2)
#define UART_TX_BUFFER_SIZE      256U
#define UART_TX_BUFFER_MASK      (UART_TX_BUFFER_SIZE - 1U)
#define UART_RX_BUFFER_SIZE      64U
#define UART_RX_BUFFER_MASK      (UART_RX_BUFFER_SIZE - 1U)
#define CMD_FRAME_MAX_LEN        8U
#define BINARY_SYNC_HEADER       0xAAU

// ============================================================================
// DATA STRUCTURES (32-BIT ALIGNED FOR CORTEX-M SINGLE-CYCLE ACCESS)
// ============================================================================

typedef struct {
    GPIO_TypeDef *port;               // GPIO Port base address (GPIOA, GPIOB, GPIOC)
    uint16_t pinIndex;                // Pin number (0 - 15)
    uint32_t pinMask;                 // Bitmask (1UL << pinIndex) for atomic BSRR/IDR
} GpioPinMap;

typedef struct {
    GpioPinMap gpio;
    bool isHardwarePwm;               // true: TIM1/TIM3 CCRx, false: TIM2 ISR Soft-PWM
    volatile uint32_t *ccrRegister;   // Pointer to Hardware Timer Capture/Compare Register
    volatile uint8_t mode;            // 0: Continuous, 1: Synchronized with masterSyncState
    volatile uint8_t level;           // Duty cycle percentage (0 - 100)
    volatile uint32_t dutyDurationUs;
    volatile uint32_t offDurationUs;
} PwmChannel;

typedef struct {
    volatile uint32_t startDelayUs;    // Time remaining until event starts (0 = active)
    volatile uint32_t remainingTimeUs; // Time remaining until event finishes
    volatile uint32_t totalDurationUs; // Total pre-calculated duration of this state
} TimelineEvent;

typedef struct {
    GpioPinMap gpio;
    bool isAnalog;                     // true: 12-bit ADC1 ISR, false: Direct GPIOx->IDR read
    uint8_t adcChannel;                // Hardware ADC1 channel number (0 - 9)
    uint32_t sampleIntervalUs;         // Sampling period in microseconds
    volatile uint32_t elapsedUs;       // Time accumulator updated inside TIM2_IRQHandler
    volatile bool adcPending;          // Queued flag for the ADC1 state machine
    volatile bool dataReady;           // Set when fresh sample is ready for UART telemetry
    volatile uint16_t lastValue;       // Latest 12-bit ADC (0-4095) or digital (0/1) reading
    volatile uint8_t streamMode;       // 0: Off, 1: ASCII, 3: 5-Byte Binary Frame (0xAA)
} SensorChannel;

// ============================================================================
// HARDWARE PIN & TASK TABLE DEFINITIONS
// ============================================================================

// Hardware PWM: Ch1 = PA8 (TIM1->CCR1), Ch3 = PB0 (TIM3->CCR3), Ch4 = PB1 (TIM3->CCR4)
// Software PWM: Ch0 = PB12, Ch2 = PB13, Ch5 = PB14 (Driven by TIM2_IRQHandler via BSRR)
static PwmChannel pwmChannels[PWM_CHANNEL_COUNT] = {
    { {GPIOB, 12, (1UL << 12)}, false, NULL,        0, 80, 0, 0 }, // Ch 0: Soft-PWM (PB12)
    { {GPIOA,  8, (1UL <<  8)}, true,  &TIM1->CCR1, 0, 80, 0, 0 }, // Ch 1: HW PWM   (PA8)
    { {GPIOB, 13, (1UL << 13)}, false, NULL,        0, 80, 0, 0 }, // Ch 2: Soft-PWM (PB13)
    { {GPIOB,  0, (1UL <<  0)}, true,  &TIM3->CCR3, 0, 80, 0, 0 }, // Ch 3: HW PWM   (PB0)
    { {GPIOB,  1, (1UL <<  1)}, true,  &TIM3->CCR4, 0, 80, 0, 0 }, // Ch 4: HW PWM   (PB1)
    { {GPIOB, 14, (1UL << 14)}, false, NULL,        0, 80, 0, 0 }  // Ch 5: Soft-PWM (PB14)
};

static TimelineEvent eventTimeline[TIMELINE_EVENT_COUNT];

// Synchronized Pulse Outputs: PB3, PB4, PB5, PB6, PB7, PB8
static const GpioPinMap pulseOutputPins[PULSE_OUTPUT_COUNT] = {
    {GPIOB, 3, (1UL << 3)}, {GPIOB, 4, (1UL << 4)}, {GPIOB, 5, (1UL << 5)},
    {GPIOB, 6, (1UL << 6)}, {GPIOB, 7, (1UL << 7)}, {GPIOB, 8, (1UL << 8)}
};

// Warning Outputs: PC13, PC14
static const GpioPinMap warningPins[WARNING_PIN_COUNT] = {
    {GPIOC, 13, (1UL << 13)},
    {GPIOC, 14, (1UL << 14)}
};

// Sensor Task Table: PA0 (ADC0), PA1 (ADC1), PA2 (Digital IDR), PA3 (Digital IDR)
static SensorChannel sensorTable[SENSOR_COUNT] = {
    { {GPIOA, 0, (1UL << 0)}, true,  0, 10000, 0, false, false, 0, 0 }, // Sensor 0: Analog 100 Hz
    { {GPIOA, 1, (1UL << 1)}, true,  1, 20000, 0, false, false, 0, 0 }, // Sensor 1: Analog 50 Hz
    { {GPIOA, 2, (1UL << 2)}, false, 2,  5000, 0, false, false, 0, 0 }, // Sensor 2: Digital 200 Hz
    { {GPIOA, 3, (1UL << 3)}, false, 3,  5000, 0, false, false, 0, 0 }  // Sensor 3: Digital 200 Hz
};

// ============================================================================
// REAL-TIME SYNCHRONIZATION, ADC & UART STATE VARIABLES (ISR-SHARED)
// ============================================================================
static volatile int32_t  syncCountdownUs        = -1;
static volatile uint32_t syncToggleAccum        = 0;
static volatile bool     syncTriggerActive      = false;
static volatile uint8_t  masterSyncState        = 0;
static volatile bool     masterStateChangedFlag = false;

static volatile uint32_t currentIsrStepUs       = 100;

// Asynchronous ADC1 State Machine Variables
static volatile int8_t   currentAdcSensorIdx    = -1;
static volatile bool     adcBusy                = false;

// Lock-Free UART1 Ring Buffers
static volatile uint8_t  uartTxBuffer[UART_TX_BUFFER_SIZE];
static volatile uint8_t  uartTxHead = 0;
static volatile uint8_t  uartTxTail = 0;

static volatile uint8_t  uartRxBuffer[UART_RX_BUFFER_SIZE];
static volatile uint8_t  uartRxHead = 0;
static volatile uint8_t  uartRxTail = 0;

static uint8_t cmdFrameBuffer[CMD_FRAME_MAX_LEN];
static uint8_t cmdFrameIndex = 0;

// ============================================================================
// LOW-LEVEL ATOMIC GPIO & CLOCK INITIALIZATION (72 MHz HSE + PLL)
// ============================================================================

/**
 * @brief Sets or resets a pin in 1 CPU cycle via STM32 BSRR register.
 */
static inline void gpioWriteAtomic(GPIO_TypeDef *port, uint32_t mask, bool state) {
    if (state) {
        port->BSRR = mask;
    } else {
        port->BSRR = (mask << 16U);
    }
}

/**
 * @brief Reads a digital input pin in 1 CPU cycle via STM32 IDR register.
 */
static inline uint16_t gpioReadAtomic(GPIO_TypeDef *port, uint32_t mask) {
    return ((port->IDR & mask) != 0U) ? 1U : 0U;
}

/**
 * @brief Helper to configure an STM32F103 GPIO pin mode in CRL (pins 0-7) or CRH (pins 8-15).
 * @param modeCnf 4-bit configuration value (MODE[1:0] + CNF[1:0])
 */
static void gpioConfigPin(GPIO_TypeDef *port, uint16_t pinIndex, uint8_t modeCnf) {
    if (pinIndex < 8U) {
        uint32_t shift = (uint32_t)pinIndex * 4U;
        port->CRL &= ~(0xFUL << shift);
        port->CRL |= ((uint32_t)(modeCnf & 0x0FU) << shift);
    } else {
        uint32_t shift = (uint32_t)(pinIndex - 8U) * 4U;
        port->CRH &= ~(0xFUL << shift);
        port->CRH |= ((uint32_t)(modeCnf & 0x0FU) << shift);
    }
}

/**
 * @brief Configures System Clock to 72 MHz from 8 MHz HSE crystal:
 *        AHB = 72 MHz, APB2 = 72 MHz, APB1 = 36 MHz (Timer clocks = 72 MHz), ADC = 12 MHz.
 */
static void initSystemClock72MHz(void) {
    // 1. Enable HSE (8 MHz external crystal) and wait until ready
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0U) {}

    // 2. Configure Flash latency (2 wait states for 72 MHz) and Prefetch Buffer
    FLASH->ACR |= FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    // 3. Configure Bus Prescalers: HCLK = SYSCLK, PCLK2 = HCLK, PCLK1 = HCLK/2, ADCCLK = PCLK2/6 (12 MHz)
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE2_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_ADCPRE_DIV6;

    // 4. Configure PLL: Source = HSE, Multiplier = x9 (8 MHz * 9 = 72 MHz)
    RCC->CFGR &= ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL);
    RCC->CFGR |= (RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9);

    // 5. Enable PLL and wait until locked
    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0U) {}

    // 6. Switch SYSCLK to PLL
    RCC->CFGR &= ~RCC_CFGR_SW;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}
}

/**
 * @brief Enables peripheral clocks and configures all GPIO pins (JTAG disabled to free PB3/PB4).
 */
static void initGpioPins(void) {
    // Enable GPIOA, GPIOB, GPIOC, and AFIO clocks
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN;

    // Disable JTAG-DP and keep SW-DP enabled (010 in SWJ_CFG) so PB3 and PB4 work as standard GPIO
    AFIO->MAPR &= ~AFIO_MAPR_SWJ_CFG;
    AFIO->MAPR |= AFIO_MAPR_SWJ_CFG_JTAGDISABLE;

    // 1. Configure PWM Pins:
    //    - Hardware PWM pins -> Alternate Function Push-Pull 50MHz (0x0B)
    //    - Software PWM pins -> General Purpose Output Push-Pull 50MHz (0x03)
    for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
        gpioWriteAtomic(pwmChannels[i].gpio.port, pwmChannels[i].gpio.pinMask, false);
        uint8_t mode = pwmChannels[i].isHardwarePwm ? 0x0BU : 0x03U;
        gpioConfigPin(pwmChannels[i].gpio.port, pwmChannels[i].gpio.pinIndex, mode);
    }

    // 2. Configure Synchronized Pulse Output Pins -> Output Push-Pull 50MHz (0x03)
    for (uint8_t i = 0; i < PULSE_OUTPUT_COUNT; i++) {
        gpioWriteAtomic(pulseOutputPins[i].port, pulseOutputPins[i].pinMask, false);
        gpioConfigPin(pulseOutputPins[i].port, pulseOutputPins[i].pinIndex, 0x03U);
    }

    // 3. Configure Warning Indicator Pins -> Output Push-Pull 2MHz (0x02)
    for (uint8_t i = 0; i < WARNING_PIN_COUNT; i++) {
        gpioWriteAtomic(warningPins[i].port, warningPins[i].pinMask, false);
        gpioConfigPin(warningPins[i].port, warningPins[i].pinIndex, 0x02U);
    }

    // 4. Configure Sensor Pins:
    //    - Analog Sensors  -> Analog Input Mode (0x00)
    //    - Digital Sensors -> Floating Input Mode (0x04)
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        uint8_t mode = sensorTable[i].isAnalog ? 0x00U : 0x04U;
        gpioConfigPin(sensorTable[i].gpio.port, sensorTable[i].gpio.pinIndex, mode);
    }
}

// ============================================================================
// BARE-METAL NON-BLOCKING USART1 ENGINE (PA9 = TX, PA10 = RX @ 115200 BAUD)
// ============================================================================

static void initUsart1Isr(uint32_t baudRate) {
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    // PA9 (TX) -> Alternate Function Push-Pull 50MHz (0x0B)
    // PA10 (RX) -> Input Pull-Up (0x08 with ODR bit set)
    gpioConfigPin(GPIOA, 9,  0x0BU);
    gpioConfigPin(GPIOA, 10, 0x08U);
    GPIOA->BSRR = (1UL << 10);

    // APB2 Clock = 72 MHz. BRR calculation with rounding:
    USART1->BRR = (72000000UL + (baudRate / 2UL)) / baudRate;

    // Enable USART1, Transmitter, Receiver, and RXNE Interrupt
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

    NVIC_SetPriority(USART1_IRQn, 2); // Priority 2 (Lower priority than TIM2 Soft-PWM)
    NVIC_EnableIRQ(USART1_IRQn);
}

void USART1_IRQHandler(void) {
    uint32_t sr = USART1->SR;

    // 1. Receive Data Register Not Empty (RXNE)
    if ((sr & USART_SR_RXNE) != 0U) {
        uint8_t rxByte = (uint8_t)(USART1->DR & 0xFFU);
        uint8_t nextHead = (uartRxHead + 1U) & UART_RX_BUFFER_MASK;
        if (nextHead != uartRxTail) {
            uartRxBuffer[uartRxHead] = rxByte;
            uartRxHead = nextHead;
        }
    }

    // 2. Transmit Data Register Empty (TXE)
    if (((sr & USART_SR_TXE) != 0U) && ((USART1->CR1 & USART_CR1_TXEIE) != 0U)) {
        if (uartTxHead == uartTxTail) {
            // Buffer empty: disable TXE interrupt
            USART1->CR1 &= ~USART_CR1_TXEIE;
        } else {
            USART1->DR = uartTxBuffer[uartTxTail];
            uartTxTail = (uartTxTail + 1U) & UART_TX_BUFFER_MASK;
        }
    }
}

static inline bool uartWriteByte(uint8_t data) {
    uint8_t nextHead = (uartTxHead + 1U) & UART_TX_BUFFER_MASK;
    if (nextHead == uartTxTail) {
        return false; // Non-blocking drop on full buffer
    }
    uartTxBuffer[uartTxHead] = data;
    uartTxHead = nextHead;

    // Enable TXE interrupt to trigger hardware transmission immediately
    USART1->CR1 |= USART_CR1_TXEIE;
    return true;
}

static void uartWriteBytes(const uint8_t *data, uint8_t length) {
    for (uint8_t i = 0; i < length; i++) {
        if (!uartWriteByte(data[i])) break;
    }
}

static void sendSensorAsciiFrame(uint8_t sensorIdx, uint16_t value) {
    uint8_t frame[16];
    frame[0] = 'S'; frame[1] = 'E'; frame[2] = 'N'; frame[3] = 'S';
    frame[4] = ':'; frame[5] = (uint8_t)('0' + sensorIdx); frame[6] = ':';

    uint8_t pos = 7;
    if (value == 0U) {
        frame[pos++] = '0';
    } else {
        uint8_t tempDigits[5];
        uint8_t digitCount = 0;
        while (value > 0U) {
            tempDigits[digitCount++] = (uint8_t)('0' + (value % 10U));
            value /= 10U;
        }
        while (digitCount > 0U) {
            frame[pos++] = tempDigits[--digitCount];
        }
    }
    frame[pos++] = '\n';
    uartWriteBytes(frame, pos);
}

static inline void sendSensorBinaryFrame(uint8_t sensorIdx, uint16_t value) {
    uint8_t highByte = (uint8_t)(value >> 8);
    uint8_t lowByte  = (uint8_t)(value & 0xFFU);
    uint8_t checksum = BINARY_SYNC_HEADER ^ sensorIdx ^ highByte ^ lowByte;

    uint8_t packet[5] = {BINARY_SYNC_HEADER, sensorIdx, highByte, lowByte, checksum};
    uartWriteBytes(packet, 5);
}

// ============================================================================
// BARE-METAL ASYNCHRONOUS 12-BIT ADC1 ENGINE (ADC1_2_IRQHandler)
// ============================================================================

static inline void triggerNextAdcConversion(void) {
    if (adcBusy) return;

    uint8_t startIdx = (currentAdcSensorIdx >= 0) ? (uint8_t)((currentAdcSensorIdx + 1) % SENSOR_COUNT) : 0U;

    for (uint8_t count = 0; count < SENSOR_COUNT; count++) {
        uint8_t idx = (startIdx + count) % SENSOR_COUNT;
        if (sensorTable[idx].isAnalog && sensorTable[idx].adcPending) {
            sensorTable[idx].adcPending = false;
            currentAdcSensorIdx = (int8_t)idx;
            adcBusy = true;

            // Select target channel in Regular Sequence Register 3 (SQR3)
            ADC1->SQR3 = (uint32_t)(sensorTable[idx].adcChannel & 0x1FU);

            // Start conversion by writing ADON a second time (STM32F103 reference manual)
            ADC1->CR2 |= ADC_CR2_ADON;
            break;
        }
    }
}

void ADC1_2_IRQHandler(void) {
    if ((ADC1->SR & ADC_SR_EOC) != 0U) {
        // Reading ADC1->DR automatically clears the EOC flag
        uint16_t result = (uint16_t)(ADC1->DR & 0x0FFFU);

        if (currentAdcSensorIdx >= 0 && currentAdcSensorIdx < (int8_t)SENSOR_COUNT) {
            sensorTable[currentAdcSensorIdx].lastValue = result;
            sensorTable[currentAdcSensorIdx].dataReady = true;
        }

        adcBusy = false;
        triggerNextAdcConversion();
    }
}

static void initAdc1Isr(void) {
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    // Set sample time to 28.5 cycles (011) for channels 0 and 1 for stable impedance matching
    ADC1->SMPR2 |= (3UL << 0) | (3UL << 3);

    // Enable End-of-Conversion (EOC) interrupt
    ADC1->CR1 = ADC_CR1_EOCIE;

    // Power up ADC1 from power-down state
    ADC1->CR2 = ADC_CR2_ADON;

    // Short stabilization delay (~10 us at 72 MHz) before hardware calibration
    for (volatile uint32_t i = 0; i < 720U; i++) { __NOP(); }

    // Reset calibration and perform internal hardware self-calibration
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while ((ADC1->CR2 & ADC_CR2_RSTCAL) != 0U) {}
    ADC1->CR2 |= ADC_CR2_CAL;
    while ((ADC1->CR2 & ADC_CR2_CAL) != 0U) {}

    NVIC_SetPriority(ADC1_2_IRQn, 1);
    NVIC_EnableIRQ(ADC1_2_IRQn);
}

// ============================================================================
// BARE-METAL HARDWARE PWM (TIM1 & TIM3) & TIM2 DYNAMIC SCHEDULER
// ============================================================================

/**
 * @brief Initializes TIM1 (PA8 = CH1) and TIM3 (PB0 = CH3, PB1 = CH4) for 1 kHz Hardware PWM.
 *        Prescaler = 71 (1 MHz timer clock -> 1 tick = 1 us), ARR = 999 (1000 us period).
 */
static void initHardwarePwmTimers(void) {
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;

    // --- TIM1 Configuration (PA8 -> TIM1_CH1) ---
    TIM1->PSC   = 72U - 1U;                     // 72 MHz / 72 = 1 MHz (1 us resolution)
    TIM1->ARR   = DEFAULT_PWM_PERIOD_US - 1U;   // 1000 us period (1 kHz)
    TIM1->CCR1  = 0U;
    // PWM Mode 1 (110) on Channel 1 + Preload Enable
    TIM1->CCMR1 = (6UL << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
    TIM1->CCER  = TIM_CCER_CC1E;                // Enable CH1 Output
    TIM1->BDTR  = TIM_BDTR_MOE;                 // Main Output Enable (Required for Advanced Timer TIM1)
    TIM1->EGR   = TIM_EGR_UG;                   // Load shadow registers
    TIM1->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;   // Start TIM1

    // --- TIM3 Configuration (PB0 -> TIM3_CH3, PB1 -> TIM3_CH4) ---
    TIM3->PSC   = 72U - 1U;                     // APB1 x2 = 72 MHz / 72 = 1 MHz (1 us resolution)
    TIM3->ARR   = DEFAULT_PWM_PERIOD_US - 1U;   // 1000 us period (1 kHz)
    TIM3->CCR3  = 0U;
    TIM3->CCR4  = 0U;
    // PWM Mode 1 (110) on Channels 3 and 4 + Preload Enable
    TIM3->CCMR2 = (6UL << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE |
                  (6UL << TIM_CCMR2_OC4M_Pos) | TIM_CCMR2_OC4PE;
    TIM3->CCER  = TIM_CCER_CC3E | TIM_CCER_CC4E;
    TIM3->EGR   = TIM_EGR_UG;
    TIM3->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;   // Start TIM3
}

/**
 * @brief Updates Hardware PWM duty cycle directly via TIMx->CCRx register (0 - 1000 us).
 */
static void applyHardwarePwm(uint8_t channelIdx) {
    PwmChannel *ch = &pwmChannels[channelIdx];
    if (!ch->isHardwarePwm || ch->ccrRegister == NULL) return;

    bool isOutputEnabled = (ch->mode == 0U) || (ch->mode == 1U && masterSyncState == 1U);
    if (isOutputEnabled && ch->level > 0U) {
        *ch->ccrRegister = ch->dutyDurationUs; // Direct 0..1000 microsecond duty match
    } else {
        *ch->ccrRegister = 0U;
    }
}

/**
 * @brief Updates PWM channel parameters and rebuilds the Soft-PWM timeline safely.
 */
static void updatePwmChannel(uint8_t channelIdx, uint8_t mode, uint8_t level) {
    if (channelIdx >= PWM_CHANNEL_COUNT) return;

    uint8_t clampedLevel = (level > 100U) ? 100U : level;
    uint32_t dCycle = (DEFAULT_PWM_PERIOD_US * (uint32_t)clampedLevel) / 100UL;
    uint32_t oCycle = (clampedLevel > 0U) ? (DEFAULT_PWM_PERIOD_US - dCycle) : 0UL;

    __disable_irq();

    PwmChannel *targetCh = &pwmChannels[channelIdx];
    targetCh->mode           = mode;
    targetCh->level          = clampedLevel;
    targetCh->dutyDurationUs = dCycle;
    targetCh->offDurationUs  = oCycle;

    for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
        uint8_t dutyIdx = i * 2U;
        uint8_t offIdx  = dutyIdx + 1U;

        if (pwmChannels[i].isHardwarePwm || pwmChannels[i].level == 0U) {
            eventTimeline[dutyIdx].startDelayUs    = INACTIVE_EVENT_MARKER;
            eventTimeline[dutyIdx].remainingTimeUs = INACTIVE_EVENT_MARKER;
            eventTimeline[dutyIdx].totalDurationUs = INACTIVE_EVENT_MARKER;

            eventTimeline[offIdx].startDelayUs     = INACTIVE_EVENT_MARKER;
            eventTimeline[offIdx].remainingTimeUs  = INACTIVE_EVENT_MARKER;
            eventTimeline[offIdx].totalDurationUs  = INACTIVE_EVENT_MARKER;

            if (!pwmChannels[i].isHardwarePwm) {
                gpioWriteAtomic(pwmChannels[i].gpio.port, pwmChannels[i].gpio.pinMask, false);
            }
        } else {
            uint32_t chDuty = pwmChannels[i].dutyDurationUs;
            uint32_t chOff  = pwmChannels[i].offDurationUs;

            eventTimeline[dutyIdx].startDelayUs    = 0U;
            eventTimeline[dutyIdx].remainingTimeUs = chDuty;
            eventTimeline[dutyIdx].totalDurationUs = chDuty;

            eventTimeline[offIdx].startDelayUs     = chDuty;
            eventTimeline[offIdx].remainingTimeUs  = chDuty + chOff;
            eventTimeline[offIdx].totalDurationUs  = chOff;

            gpioWriteAtomic(pwmChannels[i].gpio.port, pwmChannels[i].gpio.pinMask, false);
        }
    }

    __enable_irq();

    if (targetCh->isHardwarePwm) {
        applyHardwarePwm(channelIdx);
    }
}

/**
 * @brief Initializes TIM2 at 1 MHz (1 us resolution) for dynamic next-event ISR scheduling.
 */
static void initTim2DynamicScheduler(void) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    TIM2->CR1  = 0U;
    TIM2->PSC  = 72U - 1U;     // 72 MHz / 72 = 1 MHz (1 tick = 1 us)
    currentIsrStepUs = 100U;
    TIM2->ARR  = 100U - 1U;    // Initial 100 us slice
    TIM2->CNT  = 0U;
    TIM2->EGR  = TIM_EGR_UG;   // Load prescaler immediately
    TIM2->SR   = 0U;           // Clear pending update flag
    TIM2->DIER = TIM_DIER_UIE; // Enable Update Interrupt

    NVIC_SetPriority(TIM2_IRQn, 0); // Highest priority (0) for zero-jitter Soft-PWM
    NVIC_EnableIRQ(TIM2_IRQn);

    TIM2->CR1 |= TIM_CR1_CEN;  // Start TIM2
}

/**
 * @brief TIM2 Real-Time Scheduler ISR
 *        Advances the Soft-PWM timeline, reads digital sensors via IDR, queues ADC1 tasks,
 *        and dynamically updates TIM2->ARR for the exact duration of the next event slice.
 */
void TIM2_IRQHandler(void) {
    if ((TIM2->SR & TIM_SR_UIF) == 0U) return;
    TIM2->SR = ~TIM_SR_UIF; // Clear Update Interrupt Flag

    uint32_t stepExecutedUs = currentIsrStepUs;
    uint32_t nextTimeSliceUs = INACTIVE_EVENT_MARKER;

    // 1. Advance Soft-PWM Timeline and toggle pins atomically via BSRR
    for (uint8_t i = 0; i < TIMELINE_EVENT_COUNT; i++) {
        uint8_t channelIdx = i >> 1U;
        if (pwmChannels[channelIdx].isHardwarePwm) continue;

        TimelineEvent *ev = &eventTimeline[i];
        if (ev->remainingTimeUs >= INACTIVE_EVENT_MARKER) continue;

        if (ev->startDelayUs == 0U && ev->remainingTimeUs != 0U) {
            ev->remainingTimeUs = (ev->remainingTimeUs > stepExecutedUs) ? (ev->remainingTimeUs - stepExecutedUs) : 0U;

            if ((i & 1U) == 0U) {
                bool outputActive = (pwmChannels[channelIdx].mode == 0U) ||
                                    (pwmChannels[channelIdx].mode == 1U && masterSyncState == 1U);
                gpioWriteAtomic(pwmChannels[channelIdx].gpio.port, pwmChannels[channelIdx].gpio.pinMask, outputActive);

                if (ev->remainingTimeUs == 0U) {
                    ev->startDelayUs    = eventTimeline[i + 1U].totalDurationUs;
                    ev->remainingTimeUs = ev->totalDurationUs + ev->startDelayUs;
                }
            } else {
                if (ev->remainingTimeUs == 0U) {
                    ev->startDelayUs    = eventTimeline[i - 1U].totalDurationUs;
                    ev->remainingTimeUs = ev->totalDurationUs + ev->startDelayUs;
                }
                gpioWriteAtomic(pwmChannels[channelIdx].gpio.port, pwmChannels[channelIdx].gpio.pinMask, false);
            }
        } else if (ev->startDelayUs > 0U) {
            ev->startDelayUs    = (ev->startDelayUs > stepExecutedUs) ? (ev->startDelayUs - stepExecutedUs) : 0U;
            ev->remainingTimeUs = (ev->remainingTimeUs > stepExecutedUs) ? (ev->remainingTimeUs - stepExecutedUs) : 0U;
        }

        if (ev->startDelayUs == 0U && ev->remainingTimeUs > 0U && ev->remainingTimeUs < nextTimeSliceUs) {
            nextTimeSliceUs = ev->remainingTimeUs;
        }
    }

    if (nextTimeSliceUs == INACTIVE_EVENT_MARKER || nextTimeSliceUs == 0U) {
        nextTimeSliceUs = 500U;
    }
    if (nextTimeSliceUs < 10U) {
        nextTimeSliceUs = 10U;
    }

    // 2. Update Real-Time Synchronization Countdown
    if (syncCountdownUs >= 0) {
        syncCountdownUs -= (int32_t)stepExecutedUs;
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
            masterSyncState = (masterSyncState + 1U) & 1U;
            masterStateChangedFlag = true;
        }
    }

    // 4. Update Sensor Timers: Read Digital immediately via IDR, Queue Analog for ADC1 ISR
    bool newAdcQueued = false;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        SensorChannel *sensor = &sensorTable[i];
        sensor->elapsedUs += stepExecutedUs;

        if (sensor->elapsedUs >= sensor->sampleIntervalUs) {
            sensor->elapsedUs -= sensor->sampleIntervalUs;

            if (sensor->isAnalog) {
                sensor->adcPending = true;
                newAdcQueued = true;
            } else {
                sensor->lastValue = gpioReadAtomic(sensor->gpio.port, sensor->gpio.pinMask);
                sensor->dataReady = true;
            }
        }
    }

    if (newAdcQueued && !adcBusy) {
        triggerNextAdcConversion();
    }

    // 5. Reprogram TIM2 Auto-Reload Register (ARR) for the next time slice
    currentIsrStepUs = nextTimeSliceUs;
    TIM2->ARR = (uint16_t)(nextTimeSliceUs - 1U);
    TIM2->CNT = 0U;
}

// ============================================================================
// NON-BLOCKING TELEMETRY & COMMAND PARSER STATE MACHINE
// ============================================================================

static void serviceSensorTelemetry(void) {
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        if (sensorTable[i].dataReady) {
            sensorTable[i].dataReady = false;
            uint16_t capturedValue = sensorTable[i].lastValue;
            uint8_t mode = sensorTable[i].streamMode;

            if (mode == 1U) {
                sendSensorAsciiFrame(i, capturedValue);
            } else if (mode == 3U) {
                sendSensorBinaryFrame(i, capturedValue);
            }
        }
    }
}

static void serviceMasterSyncUpdates(void) {
    if (masterStateChangedFlag) {
        masterStateChangedFlag = false;
        bool state = (masterSyncState != 0U);

        // Toggle all synchronized pulse output pins via atomic BSRR
        for (uint8_t i = 0; i < PULSE_OUTPUT_COUNT; i++) {
            gpioWriteAtomic(pulseOutputPins[i].port, pulseOutputPins[i].pinMask, state);
        }

        // Update Hardware PWM channels operating in synchronized blink mode
        for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
            if (pwmChannels[i].isHardwarePwm && pwmChannels[i].mode == 1U) {
                applyHardwarePwm(i);
            }
        }
    }
}

static void executeCommandFrame(const uint8_t *frame, uint8_t length) {
    if (length < 3U) return;

    int32_t baseOffset = (int32_t)frame[0];
    int32_t cmdTarget  = (int32_t)frame[1] - baseOffset;
    int32_t param1     = (int32_t)frame[2] - baseOffset;

    if (cmdTarget < 0 || param1 < 0) return;

    // 1. Configure PWM Channels (0 to 5)
    if (cmdTarget < (int32_t)PWM_CHANNEL_COUNT && length >= 5U) {
        int32_t highNibble = (int32_t)frame[3] - baseOffset;
        int32_t lowNibble  = (int32_t)frame[4] - baseOffset;
        if (highNibble >= 0 && lowNibble >= 0) {
            uint8_t level = (uint8_t)(highNibble * 16 + lowNibble);
            updatePwmChannel((uint8_t)cmdTarget, (uint8_t)param1, level);
        }
    }
    // 2. Sensor Control & Query Commands (6 to 10)
    else if (cmdTarget < 11) {
        uint8_t sensorIdx = (uint8_t)(cmdTarget - 6);
        if (sensorIdx < SENSOR_COUNT) {
            if (param1 == 2 || param1 == 4) {
                uint16_t snapshotVal = sensorTable[sensorIdx].lastValue;
                if (param1 == 2) sendSensorAsciiFrame(sensorIdx, snapshotVal);
                else             sendSensorBinaryFrame(sensorIdx, snapshotVal);
            } else {
                sensorTable[sensorIdx].streamMode = (uint8_t)param1;
            }
        }
    }
    // 3. Warning Pins Control (11 to 12)
    else if (cmdTarget < 13) {
        uint8_t warnIdx = (uint8_t)(cmdTarget - 11);
        if (warnIdx < WARNING_PIN_COUNT) {
            gpioWriteAtomic(warningPins[warnIdx].port, warningPins[warnIdx].pinMask, (param1 > 0));
        }
    }
    // 4. Real-Time Synchronization Trigger (Command ID 25)
    else if (cmdTarget == 25 && length >= 5U) {
        int32_t param2 = (int32_t)frame[3] - baseOffset;
        int32_t param3 = (int32_t)frame[4] - baseOffset;
        if (param2 >= 0 && param3 >= 0) {
            uint32_t delayMs = (uint32_t)param3 + (uint32_t)param2 * 26UL + (uint32_t)param1 * 26UL * 26UL;
            __disable_irq();
            syncCountdownUs   = (int32_t)(delayMs * 1000UL);
            syncTriggerActive = false;
            masterSyncState   = 0U;
            __enable_irq();
        }
    }
}

static void pollUartRxStateMachine(void) {
    while (uartRxTail != uartRxHead) {
        uint8_t incomingByte = uartRxBuffer[uartRxTail];
        uartRxTail = (uartRxTail + 1U) & UART_RX_BUFFER_MASK;

        if (incomingByte == '\n' || incomingByte == '\r') {
            if (cmdFrameIndex > 0U) {
                executeCommandFrame(cmdFrameBuffer, cmdFrameIndex);
                cmdFrameIndex = 0U;
            }
        } else {
            if (cmdFrameIndex < CMD_FRAME_MAX_LEN) {
                cmdFrameBuffer[cmdFrameIndex++] = incomingByte;
            } else {
                cmdFrameIndex = 0U;
            }
        }
    }
}

// ============================================================================
// BARE-METAL C ENTRY POINT (NO ARDUINO SETUP/LOOP)
// ============================================================================

int main(void) {
    // 1. Configure 4-bit Preemption Priority Grouping (0-15 priority levels)
    NVIC_SetPriorityGrouping(3U);

    // 2. Initialize System Clock to 72 MHz (HSE 8 MHz -> PLL x9)
    initSystemClock72MHz();

    // 3. Initialize Bare-Metal GPIO Ports & Disable JTAG to release PB3/PB4
    initGpioPins();

    // 4. Initialize Hardware PWM Timers (TIM1 & TIM3 @ 1 kHz, 1 us resolution)
    initHardwarePwmTimers();

    // 5. Initialize Bare-Metal Interrupt-Driven USART1 (PA9/PA10 @ 115200 8N1)
    initUsart1Isr(115200UL);

    // 6. Initialize Default PWM Channel States (80% duty cycle, continuous mode)
    for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++) {
        updatePwmChannel(i, 0U, 80U);
    }

    // 7. Start Calibrated Asynchronous 12-Bit ADC1 & TIM2 Dynamic Scheduler
    initAdc1Isr();
    initTim2DynamicScheduler();

    // 8. Deterministic Bare-Metal Super-Loop
    while (1) {
        pollUartRxStateMachine();
        serviceMasterSyncUpdates();
        serviceSensorTelemetry();
    }
}