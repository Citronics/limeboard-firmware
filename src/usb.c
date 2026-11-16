// SPDX-License-Identifier: MIT
// USB device: descriptors and the DLN-2 transport.
//
// Interface 0  DLN-2 (vendor class, bulk OUT 0x01 / IN 0x81): the Linux dln2
//              driver only binds interface 0
// Interface 1-2  CDC ACM serial port on the header's UART (GPIO14/15)
// Interface 3  DFU runtime: "dfu-util -e" reboots into the ROM bootloader
// That is every endpoint the F401's OTG FS has: 3 IN and 2 OUT besides EP0.
#include "version.h"
#include <string.h>

#include "board.h"
#include "dln2.h"
#include "device/usbd_pvt.h"
#include "tusb.h"

enum { ITF_DLN2, ITF_CDC, ITF_CDC_DATA, ITF_DFU, ITF_COUNT };
enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_DLN2, STR_CDC, STR_DFU };

#define EP_DLN2_OUT 0x01
#define EP_DLN2_IN 0x81
#define EP_CDC_NOTIF 0x82
#define EP_CDC_OUT 0x02
#define EP_CDC_IN 0x83

static const tusb_desc_device_t device_desc = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	// Interface association for the CDC pair
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = USB_VID,
	.idProduct = USB_PID,
	.bcdDevice = FIRMWARE_VERSION,
	.iManufacturer = STR_MANUFACTURER,
	.iProduct = STR_PRODUCT,
	.iSerialNumber = STR_SERIAL,
	.bNumConfigurations = 1,
};

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + 9 + 7 + 7 + TUD_CDC_DESC_LEN + TUD_DFU_RT_DESC_LEN)

static const uint8_t config_desc[] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0, 100),
	// DLN-2: vendor interface with one bulk pair
	9, TUSB_DESC_INTERFACE, ITF_DLN2, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, 0, 0, STR_DLN2,
	7, TUSB_DESC_ENDPOINT, EP_DLN2_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
	7, TUSB_DESC_ENDPOINT, EP_DLN2_IN, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
	TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
	TUD_DFU_RT_DESCRIPTOR(ITF_DFU, STR_DFU, DFU_ATTR_WILL_DETACH, 1000, 64),
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&device_desc; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return config_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;
	static uint16_t desc[1 + 32];
	static const char *const strings[] = {
		[STR_MANUFACTURER] = "Citronics",
		[STR_PRODUCT] = "Limeboard I/O",
		[STR_DLN2] = "DLN-2 GPIO/I2C/SPI/ADC",
		[STR_CDC] = "Header UART (GPIO14/15)",
		[STR_DFU] = "Limeboard I/O DFU",
	};
	char serial[25];
	const char *s;
	if (index == STR_LANG) {
		desc[1] = 0x0409;
		desc[0] = (TUSB_DESC_STRING << 8) | 4;
		return desc;
	}
	if (index == STR_SERIAL) {
		// The 96-bit unique ID, as CircuitPython reported it
		const uint8_t *uid = (const uint8_t *)UID_BASE;
		for (int i = 0; i < 12; i++) {
			serial[2 * i] = "0123456789ABCDEF"[uid[i] >> 4];
			serial[2 * i + 1] = "0123456789ABCDEF"[uid[i] & 15];
		}
		serial[24] = 0;
		s = serial;
	} else if (index < sizeof(strings) / sizeof(strings[0]) && strings[index]) {
		s = strings[index];
	} else {
		return NULL;
	}
	size_t n = strlen(s);
	if (n > 32)
		n = 32;
	for (size_t i = 0; i < n; i++)
		desc[1 + i] = (uint8_t)s[i];
	desc[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
	return desc;
}

void tud_dfu_runtime_reboot_to_dfu_cb(void)
{
	extern void bootloader_request(void);
	bootloader_request();
}

// --- DLN-2 transport ------------------------------------------------------
//
// Requests arrive as a byte stream (the host sends no zero-length packet
// after a multiple of 64 bytes) and are cut by the header's size field.
// Each reply or event goes out as one transfer of its own, ended by a short
// packet or a zero-length packet: the host drops anything else.

#define TX_SLOTS 8

static uint8_t rx_packet[64];
static uint8_t rx_buf[1024];
static uint16_t rx_len;
static bool rx_armed;

static uint8_t tx_buf[TX_SLOTS][DLN2_MAX_MSG];
static uint16_t tx_size[TX_SLOTS];
static uint8_t tx_head, tx_tail, tx_count;
static bool tx_busy, tx_zlp;
static bool dln2_open;

static void rx_arm(void)
{
	if (dln2_open && !rx_armed && sizeof(rx_buf) - rx_len >= sizeof(rx_packet))
		rx_armed = usbd_edpt_xfer(0, EP_DLN2_OUT, rx_packet, sizeof(rx_packet));
}

static void tx_kick(void)
{
	if (!dln2_open || tx_busy || !tx_count)
		return;
	tx_busy = usbd_edpt_xfer(0, EP_DLN2_IN, tx_buf[tx_tail], tx_size[tx_tail]);
}

bool dln2_tx_space(void) { return tx_count < TX_SLOTS; }

bool dln2_send(const uint8_t *msg, uint16_t size)
{
	if (!dln2_open || tx_count == TX_SLOTS || size > DLN2_MAX_MSG)
		return false;
	memcpy(tx_buf[tx_head], msg, size);
	tx_size[tx_head] = size;
	tx_head = (tx_head + 1) % TX_SLOTS;
	tx_count++;
	tx_kick();
	return true;
}

void dln2_transport_task(void)
{
	// One request at a time, and only with room for its reply
	while (rx_len >= DLN2_HDR_SIZE && dln2_tx_space()) {
		uint16_t size = rx_buf[0] | (rx_buf[1] << 8);
		if (size < DLN2_HDR_SIZE || size > DLN2_MAX_MSG) {
			rx_len = 0;  // lost framing: start again
			break;
		}
		if (rx_len < size)
			break;
		dln2_handle(rx_buf, size);
		memmove(rx_buf, rx_buf + size, rx_len - size);
		rx_len -= size;
	}
	rx_arm();
}

static void dln2d_init(void) {}
static bool dln2d_deinit(void) { return true; }

static void dln2d_reset(uint8_t rhport)
{
	(void)rhport;
	dln2_open = rx_armed = tx_busy = tx_zlp = false;
	rx_len = tx_head = tx_tail = tx_count = 0;
}

static uint16_t dln2d_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
	if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC || itf->bInterfaceNumber != ITF_DLN2)
		return 0;
	uint16_t len = sizeof(tusb_desc_interface_t) + 2 * sizeof(tusb_desc_endpoint_t);
	if (max_len < len)
		return 0;
	uint8_t ep_out, ep_in;
	if (!usbd_open_edpt_pair(rhport, tu_desc_next(itf), 2, TUSB_XFER_BULK, &ep_out, &ep_in))
		return 0;
	dln2_open = true;
	rx_arm();
	return len;
}

static bool dln2d_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
	(void)rhport;
	(void)stage;
	(void)request;
	return false;
}

static bool dln2d_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred)
{
	(void)rhport;
	(void)result;
	if (ep_addr == EP_DLN2_OUT) {
		rx_armed = false;
		memcpy(rx_buf + rx_len, rx_packet, xferred);
		rx_len += xferred;
		rx_arm();
		return true;
	}
	if (ep_addr == EP_DLN2_IN) {
		tx_busy = false;
		if (!tx_zlp && xferred && xferred % 64 == 0) {
			tx_zlp = true;  // end of this message for the host
			tx_busy = usbd_edpt_xfer(0, EP_DLN2_IN, NULL, 0);
			return true;
		}
		tx_zlp = false;
		tx_tail = (tx_tail + 1) % TX_SLOTS;
		tx_count--;
		tx_kick();
		return true;
	}
	return false;
}

static const usbd_class_driver_t dln2_driver = {
	.name = "DLN2",
	.init = dln2d_init,
	.deinit = dln2d_deinit,
	.reset = dln2d_reset,
	.open = dln2d_open,
	.control_xfer_cb = dln2d_control_xfer_cb,
	.xfer_cb = dln2d_xfer_cb,
	.sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
	*count = 1;
	return &dln2_driver;
}
