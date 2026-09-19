#define SBRK_ERROR ((char *)-1)

struct stat;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int *);
int pipe(int *);
int write(int, const void *, int);
int read(int, void *, int);
int close(int);
struct vmconfig;
struct vminfo;
int vm_create(struct vmconfig *);
int vm_ctl(int, int);
int vm_run(int, int);
int vm_destroy(int);
int vm_cons(int, int, void *, int);
int vm_info(int, struct vminfo *);
int vm_stats(uint64 *, int);
struct hwinfo;
int hwinfo(struct hwinfo *);
struct netinfo;
int socket(int);
int bind(int, int);
int connect(int, uint32, int);
int listen(int, int);
int accept(int, uint32 *, int *);
int sendto(int, const void *, int, uint32, int);
int recvfrom(int, void *, int, uint32 *, int *, int);
int netinfo(struct netinfo *);
// user/net.c
int dns_resolve(const char *name, uint32 *ip);
int parse_ip(const char *s, uint32 *ip);
char *ip2str(uint32 ip, char *buf);
int tcp_open(const char *host, int port);
int read_all(int fd, char *buf, int max);
int consraw(int);
int kill(int);
int exec(const char *, char **);
int open(const char *, int);
int mknod(const char *, short, short);
int unlink(const char *);
int fstat(int fd, struct stat *);
int link(const char *, const char *);
int mkdir(const char *);
int chdir(const char *);
int dup(int);
int getpid(void);
char *sys_sbrk(int, int);
int pause(int);
int uptime(void);
struct timespec;
int clock_gettime(int clock, struct timespec *ts);
long sysconf(int name);               // kernel/sysconf.h

// time.c (kernel/time.h has the clocks and struct timespec)
struct tm {
  int sec, min, hour;  // 0-59, 0-59, 0-23
  int mday, mon, year; // 1-31, 1-12, e.g. 2026
  int wday, yday;      // 0 = Sunday; 0-365
};
long time(long *tloc);                // seconds since the epoch, -1 without a clock
void gmtime_r(long secs, struct tm *tm);
char *ctime_r(long secs, char *buf);  // "Sat Sep 19 15:30:12 UTC 2026", 32 bytes
long timegm(struct tm *tm);
int sync(void);

// ulib.c
int stat(const char *, struct stat *);
char *strcpy(char *, const char *);
void *memmove(void *, const void *, int);
char *strchr(const char *, char c);
int strcmp(const char *, const char *);
char *gets(char *, int max);
uint strlen(const char *);
void *memset(void *, int, uint);
int atoi(const char *);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
char *sbrk(int);
char *sbrklazy(int);

// printf.c
void fprintf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void printf(const char *, ...) __attribute__((format(printf, 1, 2)));

// umalloc.c
void *malloc(uint);
void free(void *);
