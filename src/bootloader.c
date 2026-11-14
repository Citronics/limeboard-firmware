// Entering the STM32's ROM bootloader (USB DFU, 0483:df11) without a jumper:
// on a DFU detach from the host, or when the user button is held at power-on.
// It is entered straight after reset, before any peripheral is set up.
#include "board.h"

#define BOOTLOADER_MAGIC 0xB007DF00u
#define SYSTEM_MEMORY 0x1FFF0000u

// Survives a reset (not cleared by the startup code)
static uint32_t bootloader_request_word __attribute__((section(".noinit")));

void bootloader_request(void)
{
	bootloader_request_word = BOOTLOADER_MAGIC;
	NVIC_SystemReset();
}

static void jump_to_rom(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
	(void)RCC->APB2ENR;
	SYSCFG->MEMRMP = SYSCFG_MEMRMP_MEM_MODE_0;  // system memory at address 0
	const uint32_t *rom = (const uint32_t *)SYSTEM_MEMORY;
	__set_MSP(rom[0]);
	((void (*)(void))rom[1])();
	while (1) {
	}
}

void bootloader_check(void)
{
	if (bootloader_request_word == BOOTLOADER_MAGIC) {
		bootloader_request_word = 0;
		jump_to_rom();
	}
	bootloader_request_word = 0;

	// The user button reads high while pressed: held for 50 ms at power-on
	pin_t btn = gpio_line[LINE_BTN];
	pin_mode(btn, PIN_INPUT);
	for (volatile uint32_t i = 0; i < 160000; i++)  // ~50 ms on the 16 MHz HSI
		if (!pin_read(btn))
			return;
	jump_to_rom();
}
