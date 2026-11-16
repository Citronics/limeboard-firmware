// SPDX-License-Identifier: MIT
// DLN-2 I2C: two ports, as on a Raspberry Pi
//   port 0: the STM32's I2C1, SDA GPIO2 = PB9, SCL GPIO3 = PB6 (Pi i2c1)
//   port 1: the STM32's I2C3, SDA GPIO0 = PB4, SCL GPIO1 = PA8 (Pi i2c0)
// both at 100 kHz.
//
// Each command is one complete transaction, START to STOP: Linux sends a
// register read as a write followed by a separate read. A port's pins
// become I2C on its first transfer, so they stay plain GPIO until then.
#include "board.h"
#include "dln2.h"

#define CMD_GET_PORT_COUNT 0x0300
#define CMD_ENABLE 0x0301
#define CMD_DISABLE 0x0302
#define CMD_WRITE 0x0306
#define CMD_READ 0x0307

#define MAX_LEN 256
#define WAIT_MS 20

struct port {
	I2C_TypeDef *i2c;
	uint32_t rcc_en;             // RCC->APB1ENR bit
	uint8_t lines[2];            // SDA, SCL
	uint8_t af[2];
	bool claimed;
};

static struct port ports[] = {
	{I2C1, RCC_APB1ENR_I2C1EN, {2, 3}, {4, 4}, false},
	{I2C3, RCC_APB1ENR_I2C3EN, {0, 1}, {9, 4}, false},
};
#define PORTS (sizeof(ports) / sizeof(ports[0]))

// The port of the current request
static I2C_TypeDef *I2C;

static void i2c_setup(void)
{
	I2C->CR1 = I2C_CR1_SWRST;
	I2C->CR1 = 0;
	I2C->CR2 = 42;            // APB1 MHz
	I2C->CCR = 210;           // 100 kHz standard mode: 42 MHz / (2 * 100 kHz)
	I2C->TRISE = 43;          // 1000 ns rise time + 1
	I2C->CR1 = I2C_CR1_PE;
}

static bool claim(struct port *p)
{
	if (p->claimed)
		return true;
	if (!lines_claim(p->lines, 2, OWNER_I2C))
		return false;
	// Open drain with the internal pull-ups, on top of any on the board
	pin_af(gpio_line[p->lines[0]], p->af[0], true, true);
	pin_af(gpio_line[p->lines[1]], p->af[1], true, true);
	RCC->APB1ENR |= p->rcc_en;
	(void)RCC->APB1ENR;
	i2c_setup();
	p->claimed = true;
	return true;
}

static bool wait_flag(volatile uint32_t *reg, uint32_t flag)
{
	uint32_t start = millis();
	while (!(*reg & flag)) {
		if (I2C->SR1 & I2C_SR1_AF)
			return false;
		if (millis() - start > WAIT_MS)
			return false;
	}
	return true;
}

// End a failed transaction and leave the bus usable
static uint16_t fail(void)
{
	uint16_t result = (I2C->SR1 & I2C_SR1_AF) ? DLN2_RES_NACK : DLN2_RES_TIMEOUT;
	I2C->CR1 |= I2C_CR1_STOP;
	I2C->SR1 = 0;
	if (result == DLN2_RES_TIMEOUT)
		i2c_setup();  // a stuck peripheral: reset it
	return result;
}

static uint16_t start(uint8_t addr, bool read)
{
	uint32_t t = millis();
	while (I2C->SR2 & I2C_SR2_BUSY)
		if (millis() - t > WAIT_MS) {
			i2c_setup();
			break;
		}
	I2C->CR1 &= ~I2C_CR1_POS;
	I2C->CR1 |= I2C_CR1_START;
	if (!wait_flag(&I2C->SR1, I2C_SR1_SB))
		return fail();
	I2C->DR = (addr << 1) | read;
	if (!wait_flag(&I2C->SR1, I2C_SR1_ADDR))
		return fail();
	return DLN2_RES_OK;
}

static uint16_t i2c_write(uint8_t addr, const uint8_t *buf, uint16_t len)
{
	uint16_t r = start(addr, false);
	if (r)
		return r;
	(void)I2C->SR2;  // clears ADDR
	for (uint16_t i = 0; i < len; i++) {
		if (!wait_flag(&I2C->SR1, I2C_SR1_TXE))
			return fail();
		I2C->DR = buf[i];
	}
	if (len && !wait_flag(&I2C->SR1, I2C_SR1_BTF))
		return fail();
	I2C->CR1 |= I2C_CR1_STOP;
	return DLN2_RES_OK;
}

// The F4 I2C needs different sequences for 1, 2 and more bytes (RM0368)
static uint16_t i2c_read(uint8_t addr, uint8_t *buf, uint16_t len)
{
	if (len == 2)
		I2C->CR1 |= I2C_CR1_ACK | I2C_CR1_POS;
	else
		I2C->CR1 |= I2C_CR1_ACK;
	uint16_t r = start(addr, true);
	if (r)
		return r;
	if (len <= 1) {
		I2C->CR1 &= ~I2C_CR1_ACK;
		(void)I2C->SR2;
		I2C->CR1 |= I2C_CR1_STOP;
		if (len == 1) {
			if (!wait_flag(&I2C->SR1, I2C_SR1_RXNE))
				return fail();
			buf[0] = I2C->DR;
		}
		return DLN2_RES_OK;
	}
	if (len == 2) {
		(void)I2C->SR2;
		I2C->CR1 &= ~I2C_CR1_ACK;
		if (!wait_flag(&I2C->SR1, I2C_SR1_BTF))
			return fail();
		I2C->CR1 |= I2C_CR1_STOP;
		buf[0] = I2C->DR;
		buf[1] = I2C->DR;
		I2C->CR1 &= ~I2C_CR1_POS;
		return DLN2_RES_OK;
	}
	(void)I2C->SR2;
	uint16_t i = 0;
	while (len - i > 3) {
		if (!wait_flag(&I2C->SR1, I2C_SR1_RXNE))
			return fail();
		buf[i++] = I2C->DR;
	}
	// Three left: N-2 waits in DR, N-1 in the shift register
	if (!wait_flag(&I2C->SR1, I2C_SR1_BTF))
		return fail();
	I2C->CR1 &= ~I2C_CR1_ACK;
	buf[i++] = I2C->DR;
	if (!wait_flag(&I2C->SR1, I2C_SR1_BTF))
		return fail();
	I2C->CR1 |= I2C_CR1_STOP;
	buf[i++] = I2C->DR;
	buf[i++] = I2C->DR;
	return DLN2_RES_OK;
}

void dln2_i2c_reset(void)
{
	for (unsigned i = 0; i < PORTS; i++) {
		struct port *p = &ports[i];
		if (p->claimed) {
			p->i2c->CR1 = 0;
			lines_release(p->lines, 2, OWNER_I2C);
			p->claimed = false;
		}
	}
}

void dln2_i2c(const dln2_req_t *req)
{
	if (req->id == CMD_GET_PORT_COUNT) {
		uint8_t count = PORTS;
		return dln2_reply(req, DLN2_RES_OK, &count, 1);
	}
	if (req->len < 1 || req->data[0] >= PORTS)
		return dln2_err(req, DLN2_RES_BAD_PARAM);
	struct port *p = &ports[req->data[0]];
	I2C = p->i2c;

	switch (req->id) {
	case CMD_ENABLE:
	case CMD_DISABLE:
		return dln2_ok(req);
	case CMD_WRITE:
	case CMD_READ:
		break;
	default:
		return dln2_err(req, DLN2_RES_UNSUPPORTED);
	}
	// u8 port, u8 addr, u8 mem_addr_len, le32 mem_addr, le16 len, data
	if (req->len < 9)
		return dln2_err(req, DLN2_RES_BAD_PARAM);
	uint8_t addr = req->data[1] & 0x7F;
	uint16_t len = get16(req->data + 7);
	if (len > MAX_LEN || (req->id == CMD_WRITE && req->len < 9 + len))
		return dln2_err(req, DLN2_RES_BAD_PARAM);
	if (!claim(p))
		return dln2_err(req, DLN2_RES_BUSY);

	if (req->id == CMD_WRITE) {
		uint16_t r = i2c_write(addr, req->data + 9, len);
		return r ? dln2_err(req, r) : dln2_ok(req);
	}
	uint8_t out[2 + MAX_LEN];
	uint16_t r = i2c_read(addr, out + 2, len);
	if (r)
		return dln2_err(req, r);
	put16(out, len);
	dln2_reply(req, DLN2_RES_OK, out, 2 + len);
}
