# Limeboard I/O firmware

Firmware for the Limeboard's STM32F401. It makes the board's Raspberry Pi
style header appear in Linux as standard devices, through the kernel's
mainline `dln2` drivers:

| Linux device | Header (BCM numbering, as on a Raspberry Pi) |
|---|---|
| GPIO chip "dln2", lines 0-27 | GPIO0 to GPIO27 |
| GPIO lines 28-31 | board RGB LED (red, green, blue) and user button |
| I2C port 0 (Pi i2c1) | SDA GPIO2, SCL GPIO3 (100 kHz), STM32 I2C1 |
| I2C port 1 (Pi i2c0) | SDA GPIO0, SCL GPIO1 (100 kHz), STM32 I2C3 |
| SPI port 0 (Pi spi0) | MOSI GPIO10, MISO GPIO9, SCLK GPIO11, CE0 GPIO8, CE1 GPIO7; STM32 SPI1, 328 kHz-42 MHz |
| SPI port 1 (Pi spi1) | MOSI GPIO20, MISO GPIO19, SCLK GPIO21, CE0 GPIO18, CE1 GPIO17, CE2 GPIO16; STM32 SPI2, 164 kHz-21 MHz |
| IIO ADC "dln2-adc", 8 channels, 10-bit, 3.3 V | GPIO5, 12, 16, 22, 17, 25, 20, 23 |
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

Kernel: `CONFIG_MFD_DLN2`, `CONFIG_GPIO_DLN2`, `CONFIG_I2C_DLN2`,
`CONFIG_SPI_DLN2`, `CONFIG_DLN2_ADC`. The firmware uses USB ID 1209:0001
(a pid.codes test ID until the product has its own), which the driver does
not know yet:

```sh
echo "1209 0001" > /sys/bus/usb/drivers/dln2/new_id
```

A udev rule does this at boot in the Citronics images.

The second I2C and SPI ports need a kernel whose `dln2` driver asks the
adapter for its port counts and adds a bus for each ("mfd: dln2: add a cell
for each I2C and SPI port"); others use port 0 only.

## Versions

The firmware reports its version as the USB device release (`bcdDevice`,
e.g. `0101` for 1.1; `lsusb -v -d 1209:0001`, or
`/sys/bus/usb/devices/<device>/bcdDevice`). It is set in `src/version.h`.

| Version | Changes |
|---|---|
| 1.0 | GPIO, I2C, SPI, ADC, UART |
| 1.1 | second I2C port (GPIO0/1) and second SPI port (GPIO19-21) |

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
