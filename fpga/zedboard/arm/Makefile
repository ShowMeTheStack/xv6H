# the ARM's UART bridge (bare metal, Cortex-A9)
CC = arm-none-eabi-gcc
UART_REF_HZ ?= 50000000
CFLAGS = -mcpu=cortex-a9 -marm -O2 -ffreestanding -nostdlib -Wall -DUART_REF_HZ=$(UART_REF_HZ)u
bridge.elf: start.S bridge.c link.ld
	$(CC) $(CFLAGS) -T link.ld -o $@ start.S bridge.c
clean:
	rm -f bridge.elf
