// SPDX-License-Identifier: MIT
// DLN-2 request dispatch and the generic (module 0) commands.
#include <string.h>

#include "board.h"
#include "dln2.h"

#define DLN2_GENERIC_GET_VER 0x0030
#define DLN2_GENERIC_GET_SN 0x0031
// Linux refuses any other hardware version
#define DLN2_HW_VERSION 0x200

void dln2_reply(const dln2_req_t *req, uint16_t result, const uint8_t *data, uint16_t len)
{
	uint8_t msg[DLN2_MAX_MSG];
	uint16_t size = DLN2_HDR_SIZE + 2 + len;
	if (size > sizeof(msg))
		return;
	put16(msg, size);
	put16(msg + 2, req->id);
	put16(msg + 4, req->echo);
	put16(msg + 6, req->handle);
	put16(msg + 8, result);
	if (len)
		memcpy(msg + 10, data, len);
	dln2_send(msg, size);
}

bool dln2_event(uint16_t id, const uint8_t *data, uint16_t len)
{
	uint8_t msg[DLN2_MAX_MSG];
	uint16_t size = DLN2_HDR_SIZE + len;
	if (size > sizeof(msg) || !dln2_tx_space())
		return false;
	put16(msg, size);
	put16(msg + 2, id);
	put16(msg + 4, 0);
	put16(msg + 6, 0);  // event handle
	memcpy(msg + 8, data, len);
	return dln2_send(msg, size);
}

static void generic(const dln2_req_t *req)
{
	uint8_t out[4];
	switch (req->id) {
	case DLN2_GENERIC_GET_VER:
		put32(out, DLN2_HW_VERSION);
		dln2_reply(req, DLN2_RES_OK, out, 4);
		break;
	case DLN2_GENERIC_GET_SN:
		put32(out, board_serial());
		dln2_reply(req, DLN2_RES_OK, out, 4);
		break;
	default:
		dln2_err(req, DLN2_RES_UNSUPPORTED);
	}
}

void dln2_handle(const uint8_t *msg, uint16_t size)
{
	dln2_req_t req = {
		.id = get16(msg + 2),
		.echo = get16(msg + 4),
		.handle = get16(msg + 6),
		.data = msg + DLN2_HDR_SIZE,
		.len = size - DLN2_HDR_SIZE,
	};
	switch (req.id >> 8) {
	case DLN2_MODULE_GENERIC: generic(&req); break;
	case DLN2_MODULE_GPIO: dln2_gpio(&req); break;
	case DLN2_MODULE_I2C: dln2_i2c(&req); break;
	case DLN2_MODULE_SPI: dln2_spi(&req); break;
	case DLN2_MODULE_ADC: dln2_adc(&req); break;
	default: dln2_err(&req, DLN2_RES_UNSUPPORTED);
	}
}
