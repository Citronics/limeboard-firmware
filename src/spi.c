// DLN-2 SPI: the header's SPI0 (MOSI GPIO10, MISO GPIO9, SCLK GPIO11 on the
// STM32's SPI1), chip selects CE0 = GPIO8 and CE1 = GPIO7 driven as GPIO.
// The pins become SPI on the first transfer, so they stay plain GPIO until
// something uses SPI.
#include <stddef.h>

#include "board.h"
#include "dln2.h"

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
#define APB2_HZ 84000000u

// MISO, MOSI, SCLK, CE0, CE1
static const uint8_t lines[] = {9, 10, 11, 8, 7};
static const uint8_t ce_line[] = {8, 7};
static bool claimed;
static uint8_t cs = 0xFF;   // selected chip select, 0xFF: none
static uint8_t br = 7;      // SPI_CR1 BR: f = APB2 / 2^(br + 1)
static uint8_t mode;        // bit 0 CPHA, bit 1 CPOL
static bool frame16;

static void configure(void)
{
	SPI1->CR1 = 0;
	SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | ((uint32_t)br << SPI_CR1_BR_Pos) |
		    (mode & 1 ? SPI_CR1_CPHA : 0) | (mode & 2 ? SPI_CR1_CPOL : 0) |
		    (frame16 ? SPI_CR1_DFF : 0);
	SPI1->CR1 |= SPI_CR1_SPE;
}

static bool claim(void)
{
	if (claimed)
		return true;
	if (!lines_claim(lines, sizeof(lines), OWNER_SPI))
		return false;
	for (int i = 0; i < 2; i++) {
		pin_write(gpio_line[ce_line[i]], 1);  // deselected
		pin_mode(gpio_line[ce_line[i]], PIN_OUTPUT);
	}
	pin_af(gpio_line[9], 5, false, false);
	pin_af(gpio_line[10], 5, false, false);
	pin_af(gpio_line[11], 5, false, false);
	RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
	(void)RCC->APB2ENR;
	claimed = true;
	configure();
	return true;
}

static void select(bool on)
{
	if (cs < 2)
		pin_write(gpio_line[ce_line[cs]], !on);
}

// Full duplex: words of 1 or 2 bytes, little-endian in the buffers
static bool transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint16_t step = frame16 ? 2 : 1;
	uint32_t start = millis();
	for (uint16_t i = 0; i < len; i += step) {
		uint16_t w = tx ? (frame16 ? get16(tx + i) : tx[i]) : 0;
		while (!(SPI1->SR & SPI_SR_TXE))
			if (millis() - start > 100)
				return false;
		SPI1->DR = w;
		while (!(SPI1->SR & SPI_SR_RXNE))
			if (millis() - start > 100)
				return false;
		w = SPI1->DR;
		if (rx) {
			rx[i] = w;
			if (frame16)
				rx[i + 1] = w >> 8;
		}
	}
	while (SPI1->SR & SPI_SR_BSY) {
	}
	return true;
}

void dln2_spi_reset(void)
{
	if (claimed) {
		SPI1->CR1 = 0;
		lines_release(lines, sizeof(lines), OWNER_SPI);
		claimed = false;
	}
	cs = 0xFF;
}

void dln2_spi(const dln2_req_t *req)
{
	uint8_t out[2 + MAX_LEN];
	switch (req->id) {
	case CMD_ENABLE:
	case CMD_DISABLE:
	case CMD_SS_MULTI_ENABLE:
		return dln2_ok(req);
	case CMD_GET_SS_COUNT:
		put16(out, 2);
		return dln2_reply(req, DLN2_RES_OK, out, 2);
	case CMD_GET_MIN_FREQUENCY:
		put32(out, APB2_HZ / 256);
		return dln2_reply(req, DLN2_RES_OK, out, 4);
	case CMD_GET_MAX_FREQUENCY:
		put32(out, APB2_HZ / 2);
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
		cs = 0xFF;
		for (uint8_t i = 0; i < 2; i++)
			if (!(mask & (1u << i))) {
				cs = i;
				break;
			}
		return dln2_ok(req);
	}
	case CMD_SET_MODE:
		mode = req->data[1] & 3;
		if (claimed)
			configure();
		return dln2_ok(req);
	case CMD_SET_FRAME_SIZE:
		if (req->data[1] != 8 && req->data[1] != 16)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		frame16 = req->data[1] == 16;
		if (claimed)
			configure();
		return dln2_ok(req);
	case CMD_SET_FREQUENCY: {
		if (req->len < 5)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		uint32_t want = get32(req->data + 1);
		// The fastest rate not above the request
		for (br = 0; br < 7 && APB2_HZ >> (br + 1) > want; br++) {
		}
		if (claimed)
			configure();
		put32(out, APB2_HZ >> (br + 1));
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
		if (len > MAX_LEN || (has_tx && req->len < 4 + len) || (frame16 && (len & 1)))
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		if (!claim())
			return dln2_err(req, DLN2_RES_BUSY);
		select(true);
		bool ok = transfer(has_tx ? req->data + 4 : NULL, req->id == CMD_WRITE ? NULL : out + 2, len);
		if (!(attr & ATTR_LEAVE_SS_LOW) || !ok)
			select(false);
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
