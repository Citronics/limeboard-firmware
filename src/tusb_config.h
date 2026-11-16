// SPDX-License-Identifier: MIT
#pragma once

#define CFG_TUSB_MCU OPT_MCU_STM32F4
#define CFG_TUSB_OS OPT_OS_NONE
#define BOARD_TUD_RHPORT 0
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_DFU_RUNTIME 1
// DLN-2 is the application class driver in usb.c
#define CFG_TUD_VENDOR 0
