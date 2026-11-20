#include "common.h"

static void handle_stop_signal(int signal) { (void)signal; }

static void record_unserved(int sid, Shared *sh, int count) {
  lock_shared(sid);
  sh->unserved += count;
  sh->day_unserved += count;
  unlock_shared(sid);
}

static double simulated_minutes(const Config *cfg, long long elapsed_ns) {
  return cfg->minute_ns > 0 && elapsed_ns > 0
             ? (double)elapsed_ns / cfg->minute_ns
             : 0.0;
}

static int request_service(int sid, int mid, Shared *sh, int station, int dish,
                           int coffee) {
  Message req = {.type = station,
                 .user = getpid(),
                 .station = station,
                 .dish = dish,
                 .coffee = coffee,
                 .has_ticket = 0,
                 .group_size = 1};
  int idx = station - 1;
  lock_shared(sid);
  if (!sh->active) {
    unlock_shared(sid);
    return 0;
  }
  sh->queue[idx]++;
  unlock_shared(sid);

  req.enqueued_ns = monotonic_time_ns();
  if (msgsnd(mid, &req, sizeof req - sizeof(long), 0) < 0) {
    lock_shared(sid);
    if (sh->queue[idx] > 0)
      sh->queue[idx]--;
    unlock_shared(sid);
    return 0;
  }

  Reply rep;
  if (msgrcv(mid, &rep, sizeof rep - sizeof(long), (long)getpid(), 0) < 0) {
    return 0;
  }

  double wait_minutes = simulated_minutes(&sh->cfg, rep.queue_wait_ns);

  lock_shared(sid);
  sh->wait_minutes[idx] += wait_minutes;
  sh->wait_count[idx]++;
  sh->day_wait_minutes[idx] += wait_minutes;
  sh->day_wait_count[idx]++;
  unlock_shared(sid);

  return rep.ok;
}

int main(int argc, char **argv) {
  int is_dynamic = (argc > 1 && atoi(argv[1]) == 1);

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
  struct sigaction action = {.sa_handler = handle_stop_signal};
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);

  unsigned int seed = (unsigned int)getpid() ^ (unsigned int)time(NULL);
  if (!is_dynamic) {
    sem_v(sid, SEM_START);
    sem_p(sid, SEM_GATE);
  }

  int has_ticket = (rand_r(&seed) % 100) < 80 ? 1 : 0;
  int max_grp =
      sh->cfg.max_users_per_group > 0 ? sh->cfg.max_users_per_group : 1;
  int group_size = 1 + (int)(rand_r(&seed) % max_grp);

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

  if (simulation_is_active(sid, sh) && first_station == PRIMI) {
    got_first = request_service(sid, mid, sh, PRIMI, first, 0);
  }
  if (simulation_is_active(sid, sh) && first_station == SECONDI) {
    got_second = request_service(sid, mid, sh, SECONDI, second, 0);
  }
  if (simulation_is_active(sid, sh) && first_station == PRIMI) {
    got_second = request_service(sid, mid, sh, SECONDI, second, 0);
  }
  if (simulation_is_active(sid, sh) && first_station == SECONDI) {
    got_first = request_service(sid, mid, sh, PRIMI, first, 0);
  }

  if (!simulation_is_active(sid, sh)) {
    record_unserved(sid, sh, group_size);
    shmdt(sh);
    return 0;
  }
  if (!got_first && !got_second) {
    record_unserved(sid, sh, group_size);
    shmdt(sh);
    return 0;
  }

  if (simulation_is_active(sid, sh) && coffee) {
    got_coffee = request_service(sid, mid, sh, COFFEE, coffee_type, 1);
  }
  if (simulation_is_active(sid, sh) && dessert) {
    got_dessert = request_service(sid, mid, sh, COFFEE, 0, 2);
  }

  int bits = (got_first ? 1 : 0) | (got_second ? 2 : 0);
  Message pay = {.type = CASSA,
                 .user = getpid(),
                 .dish = bits,
                 .coffee = got_coffee,
                 .dessert = got_dessert,
                 .has_ticket = has_ticket,
                 .group_size = group_size};
  lock_shared(sid);
  if (!sh->active) {
    unlock_shared(sid);
    record_unserved(sid, sh, group_size);
    shmdt(sh);
    return 0;
  }
  sh->queue[3]++;
  unlock_shared(sid);
  pay.enqueued_ns = monotonic_time_ns();
  if (msgsnd(mid, &pay, sizeof pay - sizeof(long), 0) < 0) {
    lock_shared(sid);
    if (sh->queue[3] > 0)
      sh->queue[3]--;
    int active = sh->active;
    unlock_shared(sid);
    if (!active)
      record_unserved(sid, sh, group_size);
    shmdt(sh);
    return 0;
  }

  Reply ack;
  if (msgrcv(mid, &ack, sizeof ack - sizeof(long), (long)getpid(), 0) < 0) {
    if (!simulation_is_active(sid, sh))
      record_unserved(sid, sh, group_size);
    shmdt(sh);
    return 0;
  }

  double pay_wait_minutes = simulated_minutes(&sh->cfg, ack.queue_wait_ns);

  lock_shared(sid);
  sh->wait_minutes[3] += pay_wait_minutes;
  sh->wait_count[3]++;
  sh->day_wait_minutes[3] += pay_wait_minutes;
  sh->day_wait_count[3]++;
  unlock_shared(sid);

  // Prenota i posti al tavolo per tutto il gruppo
  int seats_taken = 0;
  for (int i = 0; i < group_size; i++) {
    sem_p(sid, SEM_TABLES);
    seats_taken++;
    if (!simulation_is_active(sid, sh))
      break;
  }

  if (simulation_is_active(sid, sh) && seats_taken == group_size) {
    int dishes = got_first + got_second + got_coffee + got_dessert;
    int ms = dishes * 1000;
    long ns = scale_ms(&sh->cfg, ms);
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    nanosleep(&ts, NULL);
  }

  for (int i = 0; i < seats_taken; i++) {
    sem_v(sid, SEM_TABLES);
  }

  shmdt(sh);
  return 0;
}