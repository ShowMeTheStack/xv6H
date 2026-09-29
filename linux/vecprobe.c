// vecprobe: when does a task's vector register file change? (crossck saw all-ones after
// its first sleep, always, alone or not). The file is loaded once and summed after each
// step; each step is one suspect.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/syscall.h>
static uint32_t vbuf[256], vout[256];
static void vload(void) { uint32_t *p = vbuf; long n = 256;
	asm volatile("mv a0, %0\n\tmv a1, %1\n\t.word 0x0d3572d7\n\t.word 0x0205e407\n\t" : : "r"(n), "r"(p) : "a0", "a1", "t0", "memory"); }
static uint64_t vsum(void) { uint32_t *p = vout; long n = 256; uint64_t s = 0; memset(vout, 0, sizeof vout);
	asm volatile("mv a0, %0\n\tmv a3, %1\n\t.word 0x0d3572d7\n\t.word 0x0206e427\n\t" : : "r"(n), "r"(p) : "a0", "a3", "t0", "memory");
	for (int i = 0; i < 32; i++) s = s * 1000003 + vout[i]; return s; }
static const char *what(uint64_t s) { static char b[40]; if (s == 0xeb1f7cd4fdb731b0ULL) return "vbuf"; if (s == 0xbf2f6d657366fcc0ULL) return "ALL-ONES"; if (s == 0) return "zeros"; snprintf(b, sizeof b, "%016llx", (unsigned long long)s); return b; }
int main(void)
{
	for (int i = 0; i < 256; i++) vbuf[i] = 0x1234567u * (uint32_t)(i + 1);
	vload();
	printf("after load:            %s\n", what(vsum()));
	printf("summed again:          %s\n", what(vsum()));
	for (volatile long i = 0; i < 50000000; i++) ;
	printf("after a busy loop:     %s\n", what(vsum()));
	for (int i = 0; i < 1000; i++) syscall(SYS_getpid);
	printf("after 1000 syscalls:   %s\n", what(vsum()));
	usleep(1000);
	printf("after usleep(1 ms):    %s\n", what(vsum()));
	usleep(50000);
	printf("after usleep(50 ms):   %s\n", what(vsum()));
	sleep(1);
	printf("after sleep(1):        %s\n", what(vsum()));
	char big[4096]; memset(big, 'x', sizeof big); int fd = open("/dev/null", O_WRONLY); write(fd, big, sizeof big); close(fd);
	printf("after a 4 KB write:    %s\n", what(vsum()));
	vload();
	printf("reloaded:              %s\n", what(vsum()));
	sleep(1);
	printf("after another sleep:   %s\n", what(vsum()));
	return 0;
}
