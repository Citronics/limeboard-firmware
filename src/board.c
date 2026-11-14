// Limeboard STM32F401: pin map and board setup.
#include "board.h"

// BCM GPIO number -> STM32 pin, as wired to the Raspberry Pi style header
const pin_t gpio_line[GPIO_LINES] = {
	[0] = {GPIOB, 4},   [1] = {GPIOA, 8},   [2] = {GPIOB, 9},   [3] = {GPIOB, 6},
	[4] = {GPIOC, 9},   [5] = {GPIOA, 0},   [6] = {GPIOA, 10},  [7] = {GPIOA, 4},
	[8] = {GPIOA, 1},   [9] = {GPIOA, 6},   [10] = {GPIOA, 7},  [11] = {GPIOA, 5},
	[12] = {GPIOB, 0},  [13] = {GPIOB, 13}, [14] = {GPIOA, 2},  [15] = {GPIOA, 3},
	[16] = {GPIOB, 1},  [17] = {GPIOC, 1},  [18] = {GPIOC, 6},  [19] = {GPIOB, 14},
	[20] = {GPIOC, 3},  [21] = {GPIOB, 10}, [22] = {GPIOC, 0},  [23] = {GPIOC, 4},
	[24] = {GPIOB, 5},  [25] = {GPIOC, 2},  [26] = {GPIOB, 15}, [27] = {GPIOB, 8},
	[LINE_LED_R] = {GPIOC, 11}, [LINE_LED_G] = {GPIOC, 12},
	[LINE_LED_B] = {GPIOC, 7},  [LINE_BTN] = {GPIOB, 2},
};

uint8_t line_owner[GPIO_LINES];

static volatile uint32_t ticks;

void SysTick_Handler(void) { ticks++; }
uint32_t millis(void) { return ticks; }

void delay_ms(uint32_t ms)
{
	uint32_t start = ticks;
	while (ticks - start < ms) {
	}
}

void pin_mode(pin_t p, enum pin_mode mode)
{
	uint32_t shift = p.pin * 2;
	p.port->MODER = (p.port->MODER & ~(3u << shift)) | ((uint32_t)mode << shift);
}

void pin_pull(pin_t p, uint8_t pupd)
{
	uint32_t shift = p.pin * 2;
	p.port->PUPDR = (p.port->PUPDR & ~(3u << shift)) | ((uint32_t)pupd << shift);
}

void pin_af(pin_t p, uint8_t af, bool open_drain, bool pull_up)
{
	volatile uint32_t *afr = &p.port->AFR[p.pin >> 3];
	uint32_t shift = (p.pin & 7) * 4;
	*afr = (*afr & ~(0xFu << shift)) | ((uint32_t)af << shift);
	if (open_drain)
		p.port->OTYPER |= 1u << p.pin;
	else
		p.port->OTYPER &= ~(1u << p.pin);
	p.port->OSPEEDR |= 3u << (p.pin * 2);
	pin_pull(p, pull_up ? 1 : 0);
	pin_mode(p, PIN_AF);
}

// A line nobody uses is an input without pull, as after a Pi boots
static void line_reset(uint8_t line)
{
	pin_t p = gpio_line[line];
	if (line == LINE_LED_R || line == LINE_LED_G || line == LINE_LED_B) {
		pin_write(p, 0);  // LED off
		pin_mode(p, PIN_OUTPUT);
		return;
	}
	p.port->OTYPER &= ~(1u << p.pin);
	pin_pull(p, 0);
	pin_mode(p, PIN_INPUT);
}

bool lines_claim(const uint8_t *lines, int count, enum line_owner owner)
{
	for (int i = 0; i < count; i++)
		if (line_owner[lines[i]] != OWNER_NONE && line_owner[lines[i]] != owner)
			return false;
	for (int i = 0; i < count; i++)
		line_owner[lines[i]] = owner;
	return true;
}

void lines_release(const uint8_t *lines, int count, enum line_owner owner)
{
	for (int i = 0; i < count; i++) {
		if (line_owner[lines[i]] != owner)
			continue;
		line_owner[lines[i]] = OWNER_NONE;
		line_reset(lines[i]);
	}
}

// Before anything else: the phone's power key line (PA15) comes out of
// reset as a JTAG input with a pull-up, which could press the key.
void board_early_init(void)
{
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
	(void)RCC->AHB1ENR;
	pin_t keys[] = {PIN_FP_PWRKEY, PIN_FP_BUT_UP, PIN_FP_BUT_DWN, PIN_FP_CAM_BUT};
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		pin_pull(keys[i], 0);
		pin_mode(keys[i], PIN_INPUT);
	}
}

void board_init(void)
{
	// HUB_nRST (PB3) is never touched: it resets the USB hub this chip sits on
	for (uint8_t line = 0; line < GPIO_LINES; line++)
		line_reset(line);

	// USB OTG FS on PA11/PA12, no VBUS sensing (PA9 is not wired to VBUS)
	pin_af((pin_t){GPIOA, 11}, 10, false, false);
	pin_af((pin_t){GPIOA, 12}, 10, false, false);
	RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
	(void)RCC->AHB2ENR;
}

uint32_t board_serial(void)
{
	const uint32_t *uid = (const uint32_t *)UID_BASE;
	return uid[0] ^ uid[1] ^ uid[2];
}
