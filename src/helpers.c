#include "common.h"

static void sem_change(int id, int n, short change) {
  struct sembuf op = {(unsigned short)n, change, 0};
  while (semop(id, &op, 1) == -1 && errno == EINTR) {
  }
}
void sem_p(int id, int n) { sem_change(id, n, -1); }
void sem_v(int id, int n) { sem_change(id, n, 1); }
void lock_shared(int id) { sem_p(id, SEM_MUTEX); }
void unlock_shared(int id) { sem_v(id, SEM_MUTEX); }
int simulation_is_active(int id, Shared *shared) {
  lock_shared(id);
  int active = shared->active;
  unlock_shared(id);
  return active;
}
long scale_ms(const Config *cfg, int ms) {
  long long value = (long long)ms * cfg->minute_ns / 60000;
  return value < 1000 ? 1000 : (long)value;
}
long long monotonic_time_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return 0;
  return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}
