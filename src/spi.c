// SPDX-License-Identifier: MIT
// DLN-2 SPI: two ports, as on a Raspberry Pi
//   port 0: the STM32's SPI1, MISO GPIO9, MOSI GPIO10, SCLK GPIO11,
//           chip selects CE0 GPIO8, CE1 GPIO7 (Pi spi0)
//   port 1: the STM32's SPI2, MISO GPIO19, MOSI GPIO20, SCLK GPIO21,
//           chip selects CE0 GPIO18, CE1 GPIO17, CE2 GPIO16 (Pi spi1)
// The chip selects are driven as GPIO. A port's data pins become SPI on its
// first transfer, and each chip select on its first use, so pins stay plain
// GPIO until something uses them.
#include <stddef.h>

#include "board.h"
#include "dln2.h"

#define CMD_GET_PORT_COUNT 0x0200
#define CMD_SET_SS 0x0226
#define CMD_SS_MULTI_ENABLE 0x0238
#define CMD_ENABLE 0x0211
#define CMD_DISABLE 0x0212
#define CMD_SET_MODE 0x0214
#define CMD_SET_FRAME_SIZE 0x0216
#define CMD_SET_FREQUENCY 0x0218
#define CMD_READ_WRITE 0x021A
#define CMD_READ 0x021B
#define CMD_WRITE 0x021C
#define CMD_GET_SUPPORTED_FRAME_SIZES 0x0243
#define CMD_GET_SS_COUNT 0x0244
#define CMD_GET_MIN_FREQUENCY 0x0245
#define CMD_GET_MAX_FREQUENCY 0x0246

#define ATTR_LEAVE_SS_LOW 1
#define MAX_LEN 256
#define MAX_CS 3

struct port {
	SPI_TypeDef *spi;
	volatile uint32_t *rcc_enr;  // RCC->APB1ENR or APB2ENR
	uint32_t rcc_en;
	uint32_t clock_hz;           // the peripheral's bus clock
	uint8_t af;
	uint8_t lines[3];            // MISO, MOSI, SCLK
	uint8_t ce[MAX_CS];          // chip select lines
	uint8_t cs_count;
	// state
	bool claimed;                // MISO, MOSI, SCLK are SPI
	uint8_t ce_claimed;          // mask of chip select lines in use
	uint8_t cs;                  // selected chip select, 0xFF: none
	uint8_t br;                  // SPI_CR1 BR: f = clock / 2^(br + 1)
	uint8_t mode;                // bit 0 CPHA, bit 1 CPOL
	bool frame16;
};

static struct port ports[] = {
	{SPI1, &RCC->APB2ENR, RCC_APB2ENR_SPI1EN, 84000000u, 5, {9, 10, 11}, {8, 7}, 2,
	 false, 0, 0xFF, 7, 0, false},
	{SPI2, &RCC->APB1ENR, RCC_APB1ENR_SPI2EN, 42000000u, 5, {19, 20, 21}, {18, 17, 16}, 3,
	 false, 0, 0xFF, 7, 0, false},
};
#define PORTS (sizeof(ports) / sizeof(ports[0]))

static void configure(struct port *p)
{
	p->spi->CR1 = 0;
	p->spi->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
		      ((uint32_t)p->br << SPI_CR1_BR_Pos) |
		      (p->mode & 1 ? SPI_CR1_CPHA : 0) | (p->mode & 2 ? SPI_CR1_CPOL : 0) |
		      (p->frame16 ? SPI_CR1_DFF : 0);
	p->spi->CR1 |= SPI_CR1_SPE;
}

static bool claim(struct port *p)
{
	if (!p->claimed) {
		if (!lines_claim(p->lines, 3, OWNER_SPI))
			return false;
		for (int i = 0; i < 3; i++)
			pin_af(gpio_line[p->lines[i]], p->af, false, false);
		*p->rcc_enr |= p->rcc_en;
		(void)*p->rcc_enr;
		p->claimed = true;
		configure(p);
	}
	if (p->cs < p->cs_count && !(p->ce_claimed & (1u << p->cs))) {
		pin_t ce = gpio_line[p->ce[p->cs]];
		if (!lines_claim(&p->ce[p->cs], 1, OWNER_SPI))
			return false;
		pin_write(ce, 1);  // deselected
		pin_mode(ce, PIN_OUTPUT);
		p->ce_claimed |= 1u << p->cs;
	}
	return true;
}

static void select(struct port *p, bool on)
{
	if (p->cs < p->cs_count)
		pin_write(gpio_line[p->ce[p->cs]], !on);
}

// Full duplex: words of 1 or 2 bytes, little-endian in the buffers
static bool transfer(struct port *p, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	SPI_TypeDef *spi = p->spi;
	uint16_t step = p->frame16 ? 2 : 1;
	uint32_t start = millis();
	for (uint16_t i = 0; i < len; i += step) {
		uint16_t w = tx ? (p->frame16 ? get16(tx + i) : tx[i]) : 0;
		while (!(spi->SR & SPI_SR_TXE))
			if (millis() - start > 100)
				return false;
		spi->DR = w;
		while (!(spi->SR & SPI_SR_RXNE))
			if (millis() - start > 100)
				return false;
		w = spi->DR;
		if (rx) {
			rx[i] = w;
			if (p->frame16)
				rx[i + 1] = w >> 8;
		}
	}
	while (spi->SR & SPI_SR_BSY) {
	}
	return true;
}

void dln2_spi_reset(void)
{
	for (unsigned i = 0; i < PORTS; i++) {
		struct port *p = &ports[i];
		if (p->claimed) {
			p->spi->CR1 = 0;
			lines_release(p->lines, 3, OWNER_SPI);
			p->claimed = false;
		}
		for (uint8_t c = 0; c < p->cs_count; c++)
			if (p->ce_claimed & (1u << c))
				lines_release(&p->ce[c], 1, OWNER_SPI);
		p->ce_claimed = 0;
		p->cs = 0xFF;
		p->br = 7;
		p->mode = 0;
		p->frame16 = false;
	}
}

void dln2_spi(const dln2_req_t *req)
{
	uint8_t out[2 + MAX_LEN];

	if (req->id == CMD_GET_PORT_COUNT) {
		out[0] = PORTS;
		return dln2_reply(req, DLN2_RES_OK, out, 1);
	}
	// Every other command starts with the port
	if (req->len < 1 || req->data[0] >= PORTS)
		return dln2_err(req, DLN2_RES_BAD_PARAM);
	struct port *p = &ports[req->data[0]];

	switch (req->id) {
	case CMD_ENABLE:
	case CMD_DISABLE:
	case CMD_SS_MULTI_ENABLE:
		return dln2_ok(req);
	case CMD_GET_SS_COUNT:
		put16(out, p->cs_count);
		return dln2_reply(req, DLN2_RES_OK, out, 2);
	case CMD_GET_MIN_FREQUENCY:
		put32(out, p->clock_hz / 256);
		return dln2_reply(req, DLN2_RES_OK, out, 4);
	case CMD_GET_MAX_FREQUENCY:
		put32(out, p->clock_hz / 2);
		return dln2_reply(req, DLN2_RES_OK, out, 4);
	case CMD_GET_SUPPORTED_FRAME_SIZES:
		// u8 count, then always 36 entries
		for (int i = 0; i < 37; i++)
			out[i] = 0;
		out[0] = 2;
		out[1] = 8;
		out[2] = 16;
		return dln2_reply(req, DLN2_RES_OK, out, 37);
	}
	if (req->len < 2)
		return dln2_err(req, DLN2_RES_BAD_PARAM);

	switch (req->id) {
	case CMD_SET_SS: {
		// Active-low mask: the cleared bit is the selected chip
		uint8_t mask = req->data[1];
		p->cs = 0xFF;
		for (uint8_t i = 0; i < p->cs_count; i++)
			if (!(mask & (1u << i))) {
				p->cs = i;
				break;
			}
		return dln2_ok(req);
	}
	case CMD_SET_MODE:
		p->mode = req->data[1] & 3;
		if (p->claimed)
			configure(p);
		return dln2_ok(req);
	case CMD_SET_FRAME_SIZE:
		if (req->data[1] != 8 && req->data[1] != 16)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		p->frame16 = req->data[1] == 16;
		if (p->claimed)
			configure(p);
		return dln2_ok(req);
	case CMD_SET_FREQUENCY: {
		if (req->len < 5)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		uint32_t want = get32(req->data + 1);
		// The fastest rate not above the request
		for (p->br = 0; p->br < 7 && p->clock_hz >> (p->br + 1) > want; p->br++) {
		}
		if (p->claimed)
			configure(p);
		put32(out, p->clock_hz >> (p->br + 1));
		return dln2_reply(req, DLN2_RES_OK, out, 4);
	}
	case CMD_READ_WRITE:
	case CMD_READ:
	case CMD_WRITE: {
		// u8 port, le16 size, u8 attr, data (not for READ)
		if (req->len < 4)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		uint16_t len = get16(req->data + 1);
		uint8_t attr = req->data[3];
		bool has_tx = req->id != CMD_READ;
		if (len > MAX_LEN || (has_tx && req->len < 4 + len) || (p->frame16 && (len & 1)))
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		if (!claim(p))
			return dln2_err(req, DLN2_RES_BUSY);
		select(p, true);
		bool ok = transfer(p, has_tx ? req->data + 4 : NULL,
				   req->id == CMD_WRITE ? NULL : out + 2, len);
		if (!(attr & ATTR_LEAVE_SS_LOW) || !ok)
			select(p, false);
		if (!ok)
			return dln2_err(req, DLN2_RES_TIMEOUT);
		if (req->id == CMD_WRITE)
			return dln2_ok(req);
		put16(out, len);
		return dln2_reply(req, DLN2_RES_OK, out, 2 + len);
	}
	}
	dln2_err(req, DLN2_RES_UNSUPPORTED);
}
