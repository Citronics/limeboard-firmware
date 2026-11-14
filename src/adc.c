// DLN-2 ADC: 8 channels on header pins no Raspberry Pi bus uses, 10-bit
// values (the Linux driver's fixed format: 3.3 V / 1024 per step). A pin
// is analog only while its channel is enabled.
#include "board.h"
#include "dln2.h"

#define CMD_GET_CHANNEL_COUNT 0x0601
#define CMD_ENABLE 0x0602
#define CMD_DISABLE 0x0603
#define CMD_CHANNEL_ENABLE 0x0605
#define CMD_CHANNEL_DISABLE 0x0606
#define CMD_SET_RESOLUTION 0x0608
#define CMD_CHANNEL_GET_VAL 0x060A
#define CMD_CHANNEL_GET_ALL_VAL 0x060B
#define CMD_CHANNEL_SET_CFG 0x060C
#define EVENT_CONDITION_MET 0x0610
#define CFG_ALWAYS 5

#define CHANNELS 8
// header GPIO (BCM) and STM32 ADC input of each channel
static const uint8_t ch_line[CHANNELS] = {5, 12, 16, 22, 17, 25, 20, 23};
static const uint8_t ch_input[CHANNELS] = {0, 8, 9, 10, 11, 12, 13, 14};

static uint8_t enabled_mask;
static uint16_t event_period_ms;
static uint32_t event_last;
static uint8_t event_channel;

static void adc_setup(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
	(void)RCC->APB2ENR;
	ADC->CCR = ADC_CCR_ADCPRE_0;     // 84 MHz / 4 = 21 MHz
	ADC1->SMPR1 = 0x04924924;        // 84 cycles (0b100) on channels 10-18
	ADC1->SMPR2 = 0x24924924;        // 84 cycles (0b100) on channels 0-9
	ADC1->CR2 = ADC_CR2_ADON;
}

static uint16_t convert(uint8_t ch)
{
	if (!(ADC1->CR2 & ADC_CR2_ADON))
		adc_setup();
	ADC1->SQR1 = 0;  // one conversion
	ADC1->SQR3 = ch_input[ch];
	ADC1->CR2 |= ADC_CR2_SWSTART;
	uint32_t start = millis();
	while (!(ADC1->SR & ADC_SR_EOC))
		if (millis() - start > 5)
			return 0;
	return (ADC1->DR & 0xFFF) >> 2;  // 12 to 10 bits
}

void dln2_adc_reset(void)
{
	for (uint8_t ch = 0; ch < CHANNELS; ch++)
		if (enabled_mask & (1u << ch))
			lines_release(&ch_line[ch], 1, OWNER_ADC);
	enabled_mask = 0;
	event_period_ms = 0;
}

void dln2_adc(const dln2_req_t *req)
{
	uint8_t out[2 + 2 * CHANNELS];
	switch (req->id) {
	case CMD_SET_RESOLUTION:
	case CMD_DISABLE:
		return dln2_ok(req);
	case CMD_GET_CHANNEL_COUNT:
		out[0] = CHANNELS;
		return dln2_reply(req, DLN2_RES_OK, out, 1);
	case CMD_ENABLE:
		put16(out, 0);  // no conflicting channel
		return dln2_reply(req, DLN2_RES_OK, out, 2);
	case CMD_CHANNEL_GET_ALL_VAL:
		// le16 channel mask, le16 value per channel, always all 8
		put16(out, enabled_mask);
		for (uint8_t ch = 0; ch < CHANNELS; ch++)
			put16(out + 2 + 2 * ch, (enabled_mask & (1u << ch)) ? convert(ch) : 0);
		return dln2_reply(req, DLN2_RES_OK, out, sizeof(out));
	}
	if (req->len < 2 || req->data[1] >= CHANNELS)
		return dln2_err(req, DLN2_RES_BAD_PARAM);
	uint8_t ch = req->data[1];

	switch (req->id) {
	case CMD_CHANNEL_ENABLE:
		if (!(enabled_mask & (1u << ch))) {
			if (!lines_claim(&ch_line[ch], 1, OWNER_ADC))
				return dln2_err(req, DLN2_RES_BUSY);
			pin_mode(gpio_line[ch_line[ch]], PIN_ANALOG);
			enabled_mask |= 1u << ch;
		}
		return dln2_ok(req);
	case CMD_CHANNEL_DISABLE:
		if (enabled_mask & (1u << ch)) {
			lines_release(&ch_line[ch], 1, OWNER_ADC);
			enabled_mask &= ~(1u << ch);
		}
		return dln2_ok(req);
	case CMD_CHANNEL_GET_VAL:
		if (!(enabled_mask & (1u << ch)))
			return dln2_err(req, DLN2_RES_BUSY);
		put16(out, convert(ch));
		return dln2_reply(req, DLN2_RES_OK, out, 2);
	case CMD_CHANNEL_SET_CFG:
		// u8 port, u8 chan, u8 type, le16 period_ms, le16 low, le16 high
		if (req->len < 5)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		event_channel = ch;
		event_period_ms = req->data[2] == CFG_ALWAYS ? get16(req->data + 3) : 0;
		event_last = millis();
		return dln2_ok(req);
	}
	dln2_err(req, DLN2_RES_UNSUPPORTED);
}

// Periodic events for buffered capture: Linux then reads all channels
void adc_task(void)
{
	if (!event_period_ms || millis() - event_last < event_period_ms)
		return;
	uint8_t ev[5];
	put16(ev, 0);
	ev[2] = CFG_ALWAYS;
	put16(ev + 3, convert(event_channel));
	if (dln2_event(EVENT_CONDITION_MET, ev, sizeof(ev)))
		event_last = millis();
}
