// The header's UART (TXD GPIO14 = PA2, RXD GPIO15 = PA3, USART2) as a USB
// serial port. The pins are UART while a program has the port open (DTR)
// and plain GPIO otherwise.
#include "board.h"
#include "tusb.h"

#define APB1_HZ 42000000u

static const uint8_t lines[] = {14, 15};
static bool active;
static uint32_t baud = 115200;

static uint8_t rx_ring[512];
static volatile uint16_t rx_head, rx_tail;

void USART2_IRQHandler(void)
{
	if (USART2->SR & (USART_SR_RXNE | USART_SR_ORE)) {
		uint8_t c = USART2->DR;
		uint16_t next = (rx_head + 1) % sizeof(rx_ring);
		if (next != rx_tail) {
			rx_ring[rx_head] = c;
			rx_head = next;
		}
	}
}

static void configure(cdc_line_coding_t const *lc)
{
	uint32_t cr1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
	if (lc && lc->parity == 1)
		cr1 |= USART_CR1_PCE | USART_CR1_PS | USART_CR1_M;  // odd, 8 data bits
	else if (lc && lc->parity == 2)
		cr1 |= USART_CR1_PCE | USART_CR1_M;                 // even, 8 data bits
	USART2->CR1 = 0;
	USART2->BRR = (APB1_HZ + baud / 2) / baud;
	USART2->CR2 = (lc && lc->stop_bits == 2) ? USART_CR2_STOP_1 : 0;
	USART2->CR1 = cr1;
}

static void start(void)
{
	if (active || !lines_claim(lines, 2, OWNER_UART))
		return;
	pin_af(gpio_line[14], 7, false, false);
	pin_af(gpio_line[15], 7, false, true);
	RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
	(void)RCC->APB1ENR;
	cdc_line_coding_t lc;
	tud_cdc_get_line_coding(&lc);
	configure(&lc);
	NVIC_EnableIRQ(USART2_IRQn);
	active = true;
}

static void stop(void)
{
	if (!active)
		return;
	NVIC_DisableIRQ(USART2_IRQn);
	USART2->CR1 = 0;
	lines_release(lines, 2, OWNER_UART);
	active = false;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
	(void)itf;
	(void)rts;
	if (dtr)
		start();
	else
		stop();
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *lc)
{
	(void)itf;
	if (lc->bit_rate)
		baud = lc->bit_rate;
	if (active)
		configure(lc);
}

void uart_task(void)
{
	if (!active)
		return;
	// USB -> UART
	while (tud_cdc_available() && (USART2->SR & USART_SR_TXE)) {
		uint8_t c;
		tud_cdc_read(&c, 1);
		USART2->DR = c;
	}
	// UART -> USB
	bool wrote = false;
	while (rx_tail != rx_head && tud_cdc_write_available()) {
		tud_cdc_write_char(rx_ring[rx_tail]);
		rx_tail = (rx_tail + 1) % sizeof(rx_ring);
		wrote = true;
	}
	if (wrote)
		tud_cdc_write_flush();
}
