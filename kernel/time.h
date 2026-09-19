// the clocks user programs can read (clock_gettime system call).
#define CLOCK_REALTIME  0   // nanoseconds since 1970-01-01 00:00 UTC, from the RTC
#define CLOCK_MONOTONIC 1   // nanoseconds since boot, from the time counter

struct timespec {
  long sec;
  long nsec;
};
