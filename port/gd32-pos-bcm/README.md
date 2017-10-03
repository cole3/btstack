# GD32F103 with bcm

BTstack port for GD32F103RET6 POS board and Broadcom AP6210 Bluetooth chipset
based on GNU Tools for ARM Embedded Processors and libopencm3

Requirements:
- GNU Tools for ARM Embedded Processors: https://launchpad.net/gcc-arm-embedded
- libopencm3 is automatically fetched and build from its git repository by make

Configuration:
- Sys tick 250 ms
- LED on PA5, on when MCU in Run mode and off while in Sleep mode
- Debug UART: USART2 - 9600/8/N/1, TX on PA2
- Bluetooth: USART3 with hardware flowcontrol RTS. IRQ on CTS Rising. TX PB10, RX PB11, CTS PB13 (in), RTS PB14 (out), N_SHUTDOWN PB15

Setup with AP6210:
- Connect GD32 POS Board to AP6210 (see BoosterPack pinout)
  - USART2_CTS:   PA0
  - USART2_RTS:   PA1
  - USART2_TX:    PA2
  - USART2_RX:    PA3
  - BT_CLK:       PA6
  - BT_RST:       PC2
  - BT_WAKE:      PA5
  - BT_HOST_WAKE: PA4


TODO:
- figure out how to compile multiple examples with single Makefile/folder
