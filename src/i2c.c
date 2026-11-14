// DLN-2 I2C: the header's I2C1 (SDA GPIO2 = PB9, SCL GPIO3 = PB6), 100 kHz.
//
// Each command is one complete transaction, START to STOP: Linux sends a
// register read as a write followed by a separate read. The pins become
// I2C on the first transfer, so GPIO2/3 stay plain GPIO until then.
#include "board.h"
#include "dln2.h"

#define CMD_ENABLE 0x0301
#define CMD_DISABLE 0x0302
#define CMD_WRITE 0x0306
#define CMD_READ 0x0307

#define MAX_LEN 256
#define WAIT_MS 20

static const uint8_t lines[] = {2, 3};
static bool claimed;

static void i2c_setup(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
	(void)RCC->APB1ENR;
	I2C1->CR1 = I2C_CR1_SWRST;
	I2C1->CR1 = 0;
	I2C1->CR2 = 42;            // APB1 MHz
	I2C1->CCR = 210;           // 100 kHz standard mode: 42 MHz / (2 * 100 kHz)
	I2C1->TRISE = 43;          // 1000 ns rise time + 1
	I2C1->CR1 = I2C_CR1_PE;
}

static bool claim(void)
{
	if (claimed)
		return true;
	if (!lines_claim(lines, 2, OWNER_I2C))
		return false;
	// Open drain with the internal pull-ups, on top of the board's
	pin_af(gpio_line[2], 4, true, true);
	pin_af(gpio_line[3], 4, true, true);
	i2c_setup();
	claimed = true;
	return true;
}

static bool wait_flag(volatile uint32_t *reg, uint32_t flag)
{
	uint32_t start = millis();
	while (!(*reg & flag)) {
		if (I2C1->SR1 & I2C_SR1_AF)
			return false;
		if (millis() - start > WAIT_MS)
			return false;
	}
	return true;
}

// End a failed transaction and leave the bus usable
static uint16_t fail(void)
{
	uint16_t result = (I2C1->SR1 & I2C_SR1_AF) ? DLN2_RES_NACK : DLN2_RES_TIMEOUT;
	I2C1->CR1 |= I2C_CR1_STOP;
	I2C1->SR1 = 0;
	if (result == DLN2_RES_TIMEOUT)
		i2c_setup();  // a stuck peripheral: reset it
	return result;
}

static uint16_t start(uint8_t addr, bool read)
{
	uint32_t t = millis();
	while (I2C1->SR2 & I2C_SR2_BUSY)
		if (millis() - t > WAIT_MS) {
			i2c_setup();
			break;
		}
	I2C1->CR1 &= ~I2C_CR1_POS;
	I2C1->CR1 |= I2C_CR1_START;
	if (!wait_flag(&I2C1->SR1, I2C_SR1_SB))
		return fail();
	I2C1->DR = (addr << 1) | read;
	if (!wait_flag(&I2C1->SR1, I2C_SR1_ADDR))
		return fail();
	return DLN2_RES_OK;
}

static uint16_t i2c_write(uint8_t addr, const uint8_t *buf, uint16_t len)
{
	uint16_t r = start(addr, false);
	if (r)
		return r;
	(void)I2C1->SR2;  // clears ADDR
	for (uint16_t i = 0; i < len; i++) {
		if (!wait_flag(&I2C1->SR1, I2C_SR1_TXE))
			return fail();
		I2C1->DR = buf[i];
	}
	if (len && !wait_flag(&I2C1->SR1, I2C_SR1_BTF))
		return fail();
	I2C1->CR1 |= I2C_CR1_STOP;
	return DLN2_RES_OK;
}

// The F4 I2C needs different sequences for 1, 2 and more bytes (RM0368)
static uint16_t i2c_read(uint8_t addr, uint8_t *buf, uint16_t len)
{
	if (len == 2)
		I2C1->CR1 |= I2C_CR1_ACK | I2C_CR1_POS;
	else
		I2C1->CR1 |= I2C_CR1_ACK;
	uint16_t r = start(addr, true);
	if (r)
		return r;
	if (len <= 1) {
		I2C1->CR1 &= ~I2C_CR1_ACK;
		(void)I2C1->SR2;
		I2C1->CR1 |= I2C_CR1_STOP;
		if (len == 1) {
			if (!wait_flag(&I2C1->SR1, I2C_SR1_RXNE))
				return fail();
			buf[0] = I2C1->DR;
		}
		return DLN2_RES_OK;
	}
	if (len == 2) {
		(void)I2C1->SR2;
		I2C1->CR1 &= ~I2C_CR1_ACK;
		if (!wait_flag(&I2C1->SR1, I2C_SR1_BTF))
			return fail();
		I2C1->CR1 |= I2C_CR1_STOP;
		buf[0] = I2C1->DR;
		buf[1] = I2C1->DR;
		I2C1->CR1 &= ~I2C_CR1_POS;
		return DLN2_RES_OK;
	}
	(void)I2C1->SR2;
	uint16_t i = 0;
	while (len - i > 3) {
		if (!wait_flag(&I2C1->SR1, I2C_SR1_RXNE))
			return fail();
		buf[i++] = I2C1->DR;
	}
	// Three left: N-2 waits in DR, N-1 in the shift register
	if (!wait_flag(&I2C1->SR1, I2C_SR1_BTF))
		return fail();
	I2C1->CR1 &= ~I2C_CR1_ACK;
	buf[i++] = I2C1->DR;
	if (!wait_flag(&I2C1->SR1, I2C_SR1_BTF))
		return fail();
	I2C1->CR1 |= I2C_CR1_STOP;
	buf[i++] = I2C1->DR;
	buf[i++] = I2C1->DR;
	return DLN2_RES_OK;
}

void dln2_i2c_reset(void)
{
	if (claimed) {
		I2C1->CR1 = 0;
		lines_release(lines, 2, OWNER_I2C);
		claimed = false;
	}
}

void dln2_i2c(const dln2_req_t *req)
{
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
	if (!claim())
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
