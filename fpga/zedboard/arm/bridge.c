/*
 * xv6H on the ZedBoard: the ARM's only job once Rocket runs. PS UART0 is wired over
 * EMIO to the PL 16550 that Rocket's console is; PS UART1 is the board's USB-UART
 * (/dev/ttyACM0 on the build box). Bytes are copied both ways, nothing else. Bare metal, MMU and
 * caches off, from DDR below Rocket's quarter.
 */
#include <stdint.h>
#define R(b, o) (*(volatile uint32_t *)((b) + (o)))
#define UART0 0xE0000000u
#define UART1 0xE0001000u
#define CR 0x00
#define MR 0x04
#define IDR 0x0C
#define BAUDGEN 0x18
#define RXTOUT 0x1C
#define SR 0x2C
#define FIFO 0x30
#define BAUDDIV 0x34
#define SR_RXEMPTY (1u << 1)
#define SR_TXFULL (1u << 4)
#ifndef UART_REF_HZ
#define UART_REF_HZ 50000000u
#endif
#define BAUD 115200u

static void uart_init(uint32_t b)
{
	uint32_t bdiv = 6, cd = UART_REF_HZ / (BAUD * (bdiv + 1));

	R(b, CR) = (1u << 0) | (1u << 1);		/* reset both paths */
	while (R(b, CR) & 3u)
		;
	R(b, IDR) = 0x1fff;
	R(b, MR) = 0x20;				/* 8 bits, no parity, 1 stop */
	R(b, BAUDGEN) = cd;
	R(b, BAUDDIV) = bdiv;
	R(b, RXTOUT) = 0;
	R(b, CR) = (1u << 2) | (1u << 4);		/* RX and TX enabled */
}

static void copy(uint32_t from, uint32_t to)
{
	while (!(R(from, SR) & SR_RXEMPTY) && !(R(to, SR) & SR_TXFULL))
		R(to, FIFO) = R(from, FIFO);
}

static void say(const char *s)
{
	for (; *s; s++) {
		while (R(UART1, SR) & SR_TXFULL)
			;
		R(UART1, FIFO) = (uint8_t)*s;
	}
}

int main(void)
{
	uart_init(UART0);
	uart_init(UART1);
	say("\r\nzed: bridge up (PL 16550 <-> USB-UART)\r\n");
	for (;;) {
		copy(UART0, UART1);
		copy(UART1, UART0);
	}
}
