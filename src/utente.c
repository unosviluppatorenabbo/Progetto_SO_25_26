#include "common.h"

static double elapsed_sim_minutes(const Config *cfg, struct timespec begin,
                                  struct timespec end) {
  double elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000.0 +
                      (end.tv_nsec - begin.tv_nsec);
  return cfg->minute_ns > 0 ? elapsed_ns / cfg->minute_ns : 0.0;
}

static int request_service(int sid, int mid, Shared *sh, int station, int dish,
                           int coffee) {
  Message req = {.type = station,
                 .user = getpid(),
                 .station = station,
                 .dish = dish,
                 .coffee = coffee};
  int idx = station - 1;
  struct timespec begin, end;
  clock_gettime(CLOCK_MONOTONIC, &begin);

  lock_shared(sid);
  sh->queue[idx]++;
  unlock_shared(sid);

  if (msgsnd(mid, &req, sizeof req - sizeof(long), 0) < 0) {
    return 0;
  }

  Reply rep;
  if (msgrcv(mid, &rep, sizeof rep - sizeof(long), (long)getpid(), 0) < 0) {
    return 0;
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  double wait_minutes = elapsed_sim_minutes(&sh->cfg, begin, end);

  lock_shared(sid);
  sh->wait_minutes[idx] += wait_minutes;
  sh->wait_count[idx]++;
  sh->day_wait_minutes[idx] += wait_minutes;
  sh->day_wait_count[idx]++;
  unlock_shared(sid);

  return rep.ok;
}

int main(void) {
  int sid = semget(ftok(FTOK_PATH, FTOK_SEM), NUM_SEMAFORI, 0666);
  int mid = msgget(ftok(FTOK_PATH, FTOK_MSG), 0666);
  int hid = shmget(ftok(FTOK_PATH, FTOK_SHM), sizeof(Shared), 0666);
  if (sid < 0 || mid < 0 || hid < 0) {
    return 2;
  }

  Shared *sh = shmat(hid, NULL, 0);
  if (sh == (void *)-1) {
    return 2;
  }

  unsigned int seed = (unsigned int)getpid() ^ (unsigned int)time(NULL);
  sem_v(sid, SEM_START);
  sem_p(sid, SEM_GATE);

  int first = rand_r(&seed) % sh->cfg.n_dishes[0];
  int second = rand_r(&seed) % sh->cfg.n_dishes[1];
  int coffee = rand_r(&seed) % 2;
  int dessert = rand_r(&seed) % 2;
  int coffee_type = (int)(rand_r(&seed) % 4);
  int got_first = 0;
  int got_second = 0;
  int got_coffee = 0;
  int got_dessert = 0;

  int first_station = PRIMI;
  lock_shared(sid);
  if (sh->queue[1] < sh->queue[0]) {
    first_station = SECONDI;
  }
  unlock_shared(sid);

  if (sh->active && first_station == PRIMI) {
    got_first = request_service(sid, mid, sh, PRIMI, first, 0);
  }
  if (sh->active && first_station == SECONDI) {
    got_second = request_service(sid, mid, sh, SECONDI, second, 0);
  }
  if (sh->active && first_station == PRIMI) {
    got_second = request_service(sid, mid, sh, SECONDI, second, 0);
  }
  if (sh->active && first_station == SECONDI) {
    got_first = request_service(sid, mid, sh, PRIMI, first, 0);
  }
  if (sh->active && coffee) {
    got_coffee = request_service(sid, mid, sh, COFFEE, coffee_type, 1);
  }
  if (sh->active && dessert) {
    got_dessert = request_service(sid, mid, sh, COFFEE, 0, 2);
  }

  if (!sh->active) {
    shmdt(sh);
    return 0;
  }
  if (!got_first && !got_second) {
    lock_shared(sid);
    sh->unserved++;
    sh->day_unserved++;
    unlock_shared(sid);
    shmdt(sh);
    return 0;
  }

  int bits = (got_first ? 1 : 0) | (got_second ? 2 : 0);
  Message pay = {.type = CASSA,
                 .user = getpid(),
                 .dish = bits,
                 .coffee = got_coffee,
                 .dessert = got_dessert};
  struct timespec pay_begin, pay_end;
  clock_gettime(CLOCK_MONOTONIC, &pay_begin);

  lock_shared(sid);
  sh->queue[3]++;
  unlock_shared(sid);
  if (msgsnd(mid, &pay, sizeof pay - sizeof(long), 0) < 0) {
    shmdt(sh);
    return 0;
  }

  Reply ack;
  if (msgrcv(mid, &ack, sizeof ack - sizeof(long), (long)getpid(), 0) < 0) {
    shmdt(sh);
    return 0;
  }

  clock_gettime(CLOCK_MONOTONIC, &pay_end);
  double pay_wait_minutes = elapsed_sim_minutes(&sh->cfg, pay_begin, pay_end);

  lock_shared(sid);
  sh->wait_minutes[3] += pay_wait_minutes;
  sh->wait_count[3]++;
  sh->day_wait_minutes[3] += pay_wait_minutes;
  sh->day_wait_count[3]++;
  unlock_shared(sid);

  sem_p(sid, SEM_TABLES);
  if (sh->active) {
    int dishes = got_first + got_second + got_coffee + got_dessert;
    int ms = dishes * 1000;
    long ns = scale_ms(&sh->cfg, ms);
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    nanosleep(&ts, NULL);
  }
  sem_v(sid, SEM_TABLES);

  shmdt(sh);
  return 0;
}
