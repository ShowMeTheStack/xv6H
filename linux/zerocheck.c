// zerocheck for the Linux guest: fresh anonymous memory must be zero. The
// kernel zeroes it with cbo.zero at riscv_cboz_block_size (clear_page.S);
// a size the machine no longer has leaves every other block as it was.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	long pages = argc > 1 ? atol(argv[1]) : 1024;
	long n = pages * 4096, dirty = 0, first = -1;
	unsigned char *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 2; }
	for (long i = 0; i < n; i++)
		if (p[i]) { if (first < 0) first = i; dirty++; }
	for (long i = 0; i < n; i += 4096)
		p[i] = 0xa5;
	if (dirty) {
		printf("zerocheck: DIRTY: %ld bytes of %ld pages not zero, first at page %ld offset %ld\n",
		       dirty, pages, first / 4096, first % 4096);
		return 1;
	}
	printf("zerocheck: %ld pages clean\n", pages);
	return 0;
}
