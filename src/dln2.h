// SPDX-License-Identifier: MIT
// DLN-2 protocol, as the Linux dln2 drivers (drivers/mfd/dln2.c and its
// gpio, i2c, spi and adc cells) speak it. All fields are little-endian.
//
// Message: le16 size (whole message), le16 id (cmd | module << 8),
// le16 echo, le16 handle, then for replies le16 result and the payload.
// Replies repeat the request's id, echo and handle. Events use handle 0 and
// carry no result field.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DLN2_HDR_SIZE 8
#define DLN2_MAX_MSG 512

#define DLN2_RES_OK 0x00
#define DLN2_RES_FAIL 0x83         // any result above 0x80 is an error for Linux
#define DLN2_RES_BUSY 0x84         // pin used by another function
#define DLN2_RES_BAD_PARAM 0x85
#define DLN2_RES_NACK 0xA0         // I2C: no acknowledge
#define DLN2_RES_TIMEOUT 0xA1
#define DLN2_RES_UNSUPPORTED 0x86

#define DLN2_MODULE_GENERIC 0x00
#define DLN2_MODULE_GPIO 0x01
#define DLN2_MODULE_SPI 0x02
#define DLN2_MODULE_I2C 0x03
#define DLN2_MODULE_ADC 0x06

typedef struct {
	uint16_t id;
	uint16_t echo;
	uint16_t handle;
	const uint8_t *data;  // request payload
	uint16_t len;
} dln2_req_t;

static inline uint16_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Reply to a request: result and payload
void dln2_reply(const dln2_req_t *req, uint16_t result, const uint8_t *data, uint16_t len);
static inline void dln2_ok(const dln2_req_t *req) { dln2_reply(req, DLN2_RES_OK, 0, 0); }
static inline void dln2_err(const dln2_req_t *req, uint16_t result) { dln2_reply(req, result, 0, 0); }
// Unsolicited event (handle 0)
bool dln2_event(uint16_t id, const uint8_t *data, uint16_t len);

// Transport (usb.c) and dispatch (dln2.c)
bool dln2_send(const uint8_t *msg, uint16_t size);
bool dln2_tx_space(void);
void dln2_transport_task(void);
void dln2_handle(const uint8_t *msg, uint16_t size);

// Modules
void dln2_gpio(const dln2_req_t *req);
void dln2_i2c(const dln2_req_t *req);
void dln2_spi(const dln2_req_t *req);
void dln2_adc(const dln2_req_t *req);
void gpio_task(void);
void adc_task(void);
void dln2_modules_reset(void);
