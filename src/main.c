// Limeboard I/O firmware for the STM32F401: the Raspberry Pi style header
// as standard Linux devices through the kernel's dln2 drivers (GPIO, I2C,
// SPI, ADC), the header UART as a USB serial port, and a DFU runtime
// interface to reach the ROM bootloader for updates.
#include "board.h"
#include "dln2.h"
#include "tusb.h"

uint32_t clock_init(void);
void bootloader_check(void);
void uart_task(void);
void dln2_gpio_reset(void);
void dln2_i2c_reset(void);
void dln2_spi_reset(void);
void dln2_adc_reset(void);

uint32_t tusb_time_millis_api(void) { return millis(); }

void OTG_FS_IRQHandler(void) { tud_int_handler(0); }

// The host went away or reset the device: back to the boot state
void dln2_modules_reset(void)
{
	dln2_i2c_reset();
	dln2_spi_reset();
	dln2_adc_reset();
	dln2_gpio_reset();
	for (uint8_t line = 0; line < GPIO_LINES; line++) {
		uint8_t l = line;
		lines_release(&l, 1, OWNER_GPIO);
	}
}

void tud_umount_cb(void) { dln2_modules_reset(); }

int main(void)
{
	board_early_init();
	bootloader_check();
	clock_init();
	board_init();
	dln2_gpio_reset();

	tud_init(BOARD_TUD_RHPORT);
	// No VBUS sensing: the device is always powered by its host
	USB_OTG_FS->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;
	USB_OTG_FS->GCCFG &= ~(USB_OTG_GCCFG_VBUSBSEN | USB_OTG_GCCFG_VBUSASEN);

	while (1) {
		tud_task();
		dln2_transport_task();
		gpio_task();
		adc_task();
		uart_task();
	}
}
