// Limeboard STM32F401: pins and shared helpers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stm32f4xx.h"

// USB identity. 1209:0001 is a pid.codes test ID: replace it with a PID
// allocated to the product before shipping.
#define USB_VID 0x1209
#define USB_PID 0x0001

typedef struct {
	GPIO_TypeDef *port;
	uint8_t pin;
} pin_t;

// GPIO lines seen by Linux: 0-27 are the Raspberry Pi header's BCM GPIO
// numbers, 28-31 the board's RGB LED and user button. The Linux dln2
// driver supports at most 32 lines.
#define GPIO_LINES 32
#define LINE_LED_R 28
#define LINE_LED_G 29
#define LINE_LED_B 30
#define LINE_BTN 31
extern const pin_t gpio_line[GPIO_LINES];

// Who uses a line: free (input), GPIO (Linux requested it), or a function
enum line_owner { OWNER_NONE, OWNER_GPIO, OWNER_I2C, OWNER_SPI, OWNER_UART, OWNER_ADC };
extern uint8_t line_owner[GPIO_LINES];

// Claim lines for a function: all or none, fails if any is taken
bool lines_claim(const uint8_t *lines, int count, enum line_owner owner);
void lines_release(const uint8_t *lines, int count, enum line_owner owner);

// Pin modes (MODER) and output types
enum pin_mode { PIN_INPUT = 0, PIN_OUTPUT = 1, PIN_AF = 2, PIN_ANALOG = 3 };
void pin_mode(pin_t p, enum pin_mode mode);
void pin_af(pin_t p, uint8_t af, bool open_drain, bool pull_up);
void pin_pull(pin_t p, uint8_t pupd);
static inline bool pin_read(pin_t p) { return (p.port->IDR >> p.pin) & 1; }
static inline void pin_write(pin_t p, bool v) { p.port->BSRR = v ? (1u << p.pin) : (1u << (p.pin + 16)); }

// Board signals that are not GPIO lines
#define PIN_FP_PWRKEY ((pin_t){GPIOA, 15})
#define PIN_FP_BUT_UP ((pin_t){GPIOC, 13})
#define PIN_FP_BUT_DWN ((pin_t){GPIOC, 10})
#define PIN_FP_CAM_BUT ((pin_t){GPIOC, 8})

void board_early_init(void);
void board_init(void);
uint32_t millis(void);
void delay_ms(uint32_t ms);
uint32_t board_serial(void);
