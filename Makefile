# SPDX-License-Identifier: MIT
# Limeboard I/O firmware for the STM32F401: make, then flash build/limeboard-io.bin
PROJECT = limeboard-io
CROSS ?= arm-none-eabi-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE = $(CROSS)size

TUSB = lib/tinyusb/src
DEV = lib/cmsis-device-f4
CORE = lib/cmsis-core/CMSIS/Core

ARCH = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS = $(ARCH) -Os -g -std=gnu11 -MMD -MP -Wall -Wextra -Wno-unused-parameter \
	-ffunction-sections -fdata-sections \
	-DSTM32F401xE -DHSE_VALUE=8000000 \
	-Isrc -I$(TUSB) -I$(DEV)/Include -I$(CORE)/Include
LDFLAGS = $(ARCH) -Tstm32f401xe.ld -Wl,--gc-sections -Wl,-Map=build/$(PROJECT).map \
	-specs=nano.specs -specs=nosys.specs

SRC = $(wildcard src/*.c) \
	$(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c \
	$(TUSB)/device/usbd.c $(TUSB)/device/usbd_control.c \
	$(TUSB)/class/cdc/cdc_device.c $(TUSB)/class/dfu/dfu_rt_device.c \
	$(TUSB)/portable/synopsys/dwc2/dcd_dwc2.c $(TUSB)/portable/synopsys/dwc2/dwc2_common.c \
	$(DEV)/Source/Templates/system_stm32f4xx.c
ASM = $(DEV)/Source/Templates/gcc/startup_stm32f401xe.s
OBJ = $(patsubst %,build/%.o,$(SRC) $(ASM))

all: build/$(PROJECT).bin

build/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.s.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -c $< -o $@

build/$(PROJECT).elf: $(OBJ) stm32f401xe.ld
	$(CC) $(LDFLAGS) $(OBJ) -o $@
	$(SIZE) $@

build/$(PROJECT).bin: build/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

# Into the ROM bootloader first: "dfu-util -e" on this firmware, or the
# user button held at power-on
flash: build/$(PROJECT).bin
	dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D $<

clean:
	rm -rf build

.PHONY: all flash clean

# Header dependencies written by -MMD
-include $(OBJ:.o=.d)
