# Limeboard I/O firmware

Firmware for the Limeboard's STM32F401. It makes the board's Raspberry Pi
style header appear in Linux as standard devices, through the
`citronics-dln2` driver of the Citronics kernels:

| Linux device | Header (BCM numbering, as on a Raspberry Pi) |
|---|---|
| GPIO chip "citronics-dln2", lines 0-27 | GPIO0 to GPIO27 |
| GPIO lines 28-31 | board RGB LED (red, green, blue) and user button |
| I2C port 0 (Pi i2c1) | SDA GPIO2, SCL GPIO3 (100 kHz), STM32 I2C1 |
| I2C port 1 (Pi i2c0) | SDA GPIO0, SCL GPIO1 (100 kHz), STM32 I2C3 |
| SPI port 0 (Pi spi0) | MOSI GPIO10, MISO GPIO9, SCLK GPIO11, CE0 GPIO8, CE1 GPIO7; STM32 SPI1, 328 kHz-42 MHz |
| SPI port 1 (Pi spi1) | MOSI GPIO20, MISO GPIO19, SCLK GPIO21, CE0 GPIO18, CE1 GPIO17, CE2 GPIO16; STM32 SPI2, 164 kHz-21 MHz |
| IIO ADC "citronics-dln2-adc", 8 channels, 10-bit, 3.3 V | GPIO5, 12, 16, 22, 17, 25, 20, 23 |
| USB serial port (`ttyACM`) | UART TXD GPIO14, RXD GPIO15 |

A pin is a plain GPIO until a function uses it: I2C, SPI and the ADC take
their pins on first use (an SPI chip select on its own first use), the UART
while its serial port is open. A pin in use by one function is refused to
the others, as on a Raspberry Pi, until the STM32 resets.

There is one UART: the STM32F401's other USARTs have no usable TX/RX pair
on the header, and the USB interface has no endpoints left for a second
serial port.

Not available through DLN-2: PWM, and the phone's key lines, which stay
undriven.

## Linux side

Kernel: `CONFIG_CITRONICS_DLN2`, the Limeboard I/O driver of the Citronics
kernels (`drivers/misc/citronics-dln2` in
[linux-msm8x74](https://github.com/mlainez/linux-msm8x74), 6.12 and later).
It binds to the firmware's USB ID, 1209:0001 (a pid.codes test ID until the
product has its own), and adds a bus for each I2C and SPI port the firmware
reports. The firmware speaks the Diolan DLN-2 protocol, so the kernel's own
`dln2` drivers work too, for GPIO, the first I2C and SPI ports and the ADC,
once told the ID (`echo "1209 0001" > /sys/bus/usb/drivers/dln2/new_id`).

## Versions

The firmware reports its version as the USB device release (`bcdDevice`,
e.g. `0101` for 1.1; `lsusb -v -d 1209:0001`, or
`/sys/bus/usb/devices/<device>/bcdDevice`). It is set in `src/version.h`.

| Version | Changes |
|---|---|
| 1.0 | GPIO, I2C, SPI, ADC, UART |
| 1.1 | second I2C port (GPIO0/1) and second SPI port (GPIO19-21) |
| 1.2 | GPIO interrupts on rising or falling edges only, as SPI chips such as the MCP2515 ask for |
| 1.3 | interrupts on lines that no program requested as GPIO, as kernel drivers use them |

## Updating on a Lime image

The Citronics Lime images carry this firmware in
`/usr/share/limeboard/limeboard-io.bin`:

```sh
limeboard-config firmware              # running and shipped versions
sudo limeboard-config firmware update
```

## Building and flashing

Needs `arm-none-eabi-gcc` and `dfu-util`.

```sh
git submodule update --init
make
dfu-util -d 1209:0001 -e    # this firmware: reboot into the ROM bootloader
make flash                  # write build/limeboard-io.bin
```

The ROM bootloader (USB DFU, 0483:df11) is also reached by holding the user
button while powering the board, or from CircuitPython with
`microcontroller.on_next_reset(microcontroller.RunMode.BOOTLOADER)`.

Back up the whole flash before replacing a firmware:

```sh
dfu-util -d 0483:df11 -a 0 -s 0x08000000:524288:leave -U backup.bin
```

## Design notes

- The crystal's frequency is measured at boot against the internal
  oscillator (TIM11 on HSE_RTC), so the 48 MHz USB clock does not depend
  on knowing the crystal.
- The phone's power key line (PA15) is made a plain input first thing:
  it leaves reset as a JTAG pin with a pull-up. HUB_nRST (PB3) is never
  driven: it resets the USB hub the STM32 itself sits on.
- USB: interface 0 DLN-2 (the Linux driver only binds interface 0), 1-2
  CDC serial, 3 DFU runtime. That uses every endpoint of the F401.
- Every reply goes out as a transfer of its own, ended by a short or
  zero-length packet, and within the driver's 200 ms timeout.

## License

MIT (`LICENSE`), Copyright (c) 2026 Citronics. The libraries in `lib/` keep
their own licenses: TinyUSB is MIT (`lib/tinyusb/LICENSE`), ST's CMSIS
device and core headers are Apache-2.0 (`lib/cmsis-device-f4/LICENSE.md`,
`lib/cmsis-core/LICENSE.md`).
