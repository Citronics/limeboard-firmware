// DLN-2 GPIO: the 32 lines of board.c (BCM numbering on the header).
#include "board.h"
#include "dln2.h"

#define CMD_GET_PIN_COUNT 0x0101
#define CMD_SET_DEBOUNCE 0x0104
#define CMD_PIN_GET_VAL 0x010B
#define CMD_PIN_SET_OUT_VAL 0x010C
#define CMD_PIN_GET_OUT_VAL 0x010D
#define CMD_PIN_ENABLE 0x0110
#define CMD_PIN_DISABLE 0x0111
#define CMD_PIN_SET_DIRECTION 0x0113
#define CMD_PIN_GET_DIRECTION 0x0114
#define CMD_PIN_SET_EVENT_CFG 0x011E
#define EVENT_CONDITION_MET 0x010F

// DLN-2 event types: any change, a level, or one edge
enum {
	EVENT_NONE = 0x00,
	EVENT_CHANGE = 0x01,
	EVENT_LEVEL_HIGH = 0x02,
	EVENT_LEVEL_LOW = 0x03,
	EVENT_RISING = 0x11,
	EVENT_FALLING = 0x21,
};

static bool is_output[GPIO_LINES];
static bool out_val[GPIO_LINES];
static uint8_t event_type[GPIO_LINES];
static bool level[GPIO_LINES];         // last level reported
static bool event_claim[GPIO_LINES];   // claimed by its event configuration
static uint32_t changed_at[GPIO_LINES];
static uint32_t debounce_ms;
static uint16_t event_count;

static void set_direction(uint8_t line, bool output)
{
	pin_t p = gpio_line[line];
	is_output[line] = output;
	if (output) {
		pin_write(p, out_val[line]);
		pin_mode(p, PIN_OUTPUT);
	} else {
		pin_mode(p, PIN_INPUT);
	}
}

void dln2_gpio_reset(void)
{
	for (uint8_t line = 0; line < GPIO_LINES; line++) {
		is_output[line] = (line == LINE_LED_R || line == LINE_LED_G || line == LINE_LED_B);
		out_val[line] = false;
		event_type[line] = EVENT_NONE;
		event_claim[line] = false;
	}
	debounce_ms = 0;
}

static void reply_pin(const dln2_req_t *req, uint16_t line, uint8_t value)
{
	uint8_t out[3];
	put16(out, line);
	out[2] = value;
	dln2_reply(req, DLN2_RES_OK, out, 3);
}

void dln2_gpio(const dln2_req_t *req)
{
	if (req->id == CMD_GET_PIN_COUNT) {
		uint8_t out[2];
		put16(out, GPIO_LINES);
		dln2_reply(req, DLN2_RES_OK, out, 2);
		return;
	}
	if (req->id == CMD_SET_DEBOUNCE) {
		if (req->len < 4)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		debounce_ms = (get32(req->data) + 999) / 1000;  // given in microseconds
		return dln2_ok(req);
	}
	if (req->len < 2 || get16(req->data) >= GPIO_LINES)
		return dln2_err(req, DLN2_RES_BAD_PARAM);
	uint8_t line = get16(req->data);
	pin_t p = gpio_line[line];

	switch (req->id) {
	case CMD_PIN_ENABLE: {
		uint8_t l = line;
		if (!lines_claim(&l, 1, OWNER_GPIO))
			return dln2_err(req, DLN2_RES_BUSY);
		return dln2_ok(req);
	}
	case CMD_PIN_DISABLE: {
		uint8_t l = line;
		event_type[line] = EVENT_NONE;
		event_claim[line] = false;
		lines_release(&l, 1, OWNER_GPIO);
		is_output[line] = (line == LINE_LED_R || line == LINE_LED_G || line == LINE_LED_B);
		out_val[line] = false;
		return dln2_ok(req);
	}
	case CMD_PIN_GET_DIRECTION:
		return reply_pin(req, line, is_output[line]);
	case CMD_PIN_GET_VAL:
		return reply_pin(req, line, pin_read(p));
	case CMD_PIN_GET_OUT_VAL:
		return reply_pin(req, line, out_val[line]);
	}

	// An interrupt user (a kernel driver's irq on this line) configures
	// events without requesting the line: claim it as an input then, and
	// release it when its events are turned off
	if (req->id == CMD_PIN_SET_EVENT_CFG && line_owner[line] == OWNER_NONE &&
	    req->len >= 3 && req->data[2] != EVENT_NONE) {
		uint8_t l = line;
		lines_claim(&l, 1, OWNER_GPIO);
		set_direction(line, false);
		event_claim[line] = true;
	}

	// Changing a line needs it requested (enabled) first
	if (line_owner[line] != OWNER_GPIO)
		return dln2_err(req, DLN2_RES_BUSY);
	switch (req->id) {
	case CMD_PIN_SET_DIRECTION:
		if (req->len < 3)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		if (req->data[2] && line == LINE_BTN)
			return dln2_err(req, DLN2_RES_BAD_PARAM);  // the button is an input
		set_direction(line, req->data[2]);
		return dln2_ok(req);
	case CMD_PIN_SET_OUT_VAL:
		if (req->len < 3)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		out_val[line] = req->data[2];
		if (is_output[line])
			pin_write(p, out_val[line]);
		return dln2_ok(req);
	case CMD_PIN_SET_EVENT_CFG:
		if (req->len < 3)
			return dln2_err(req, DLN2_RES_BAD_PARAM);
		event_type[line] = req->data[2];
		if (event_type[line] == EVENT_NONE && event_claim[line]) {
			uint8_t l = line;
			lines_release(&l, 1, OWNER_GPIO);
			event_claim[line] = false;
			return dln2_ok(req);
		}
		level[line] = pin_read(p);
		changed_at[line] = millis();
		// A level condition already met is reported at once
		if ((event_type[line] == EVENT_LEVEL_HIGH && level[line]) ||
		    (event_type[line] == EVENT_LEVEL_LOW && !level[line]))
			level[line] = !level[line];
		return dln2_ok(req);
	}
	dln2_err(req, DLN2_RES_UNSUPPORTED);
}

// Called every loop: report debounced changes of lines with events enabled
void gpio_task(void)
{
	static uint32_t last_ms;
	uint32_t now = millis();
	if (now == last_ms)
		return;
	last_ms = now;
	for (uint8_t line = 0; line < GPIO_LINES; line++) {
		uint8_t type = event_type[line];
		if (type == EVENT_NONE || line_owner[line] != OWNER_GPIO)
			continue;
		bool v = pin_read(gpio_line[line]);
		if (v == level[line]) {
			changed_at[line] = now;
			continue;
		}
		if (now - changed_at[line] < debounce_ms)
			continue;
		bool report = type == EVENT_CHANGE ||
			      ((type == EVENT_LEVEL_HIGH || type == EVENT_RISING) && v) ||
			      ((type == EVENT_LEVEL_LOW || type == EVENT_FALLING) && !v);
		if (report) {
			// le16 count, u8 type, le16 pin, u8 value
			uint8_t ev[6];
			put16(ev, ++event_count);
			ev[2] = type;
			put16(ev + 3, line);
			ev[5] = v;
			if (!dln2_event(EVENT_CONDITION_MET, ev, sizeof(ev)))
				continue;  // no room yet: retry next millisecond
		}
		level[line] = v;
	}
}
