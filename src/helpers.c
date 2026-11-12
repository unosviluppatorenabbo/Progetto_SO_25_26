#include "common.h"

static void sem_change(int id, int n, short change) {
    struct sembuf op = {(unsigned short)n, change, 0};
    while (semop(id, &op, 1) == -1 && errno == EINTR) {}
}
void sem_p(int id, int n) { sem_change(id, n, -1); }
void sem_v(int id, int n) { sem_change(id, n, 1); }
void lock_shared(int id) { sem_p(id, SEM_MUTEX); }
void unlock_shared(int id) { sem_v(id, SEM_MUTEX); }
long scale_ms(const Config *cfg, int ms) {
    long long value = (long long)ms * cfg->minute_ns / 60000;
    return value < 1000 ? 1000 : (long)value;
}
