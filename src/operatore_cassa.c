#include "common.h"

static void handle_stop_signal(int signal) { (void)signal; }

int main(void) {
  int sid = semget(ftok(FTOK_PATH, FTOK_SEM), NUM_SEMAFORI, 0666),
      mid = msgget(ftok(FTOK_PATH, FTOK_MSG), 0666),
      hid = shmget(ftok(FTOK_PATH, FTOK_SHM), sizeof(Shared), 0666);
  if (sid < 0 || mid < 0 || hid < 0)
    return 2;
  Shared *sh = shmat(hid, NULL, 0);
  if (sh == (void *)-1)
    return 2;
  struct sigaction action = {.sa_handler = handle_stop_signal};
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  unsigned int seed = (unsigned int)getpid() ^ (unsigned int)time(NULL);
  int pauses_taken = 0;
  int station_sem = SEM_STATION_CASSA;
  int holds_station = 0;
  int counted_worker = 0;
  sem_v(sid, SEM_START);
  sem_p(sid, SEM_GATE);
  while (simulation_is_active(sid, sh)) {
    if (!holds_station) {
      sem_p(sid, station_sem);
      lock_shared(sid);
      int active = sh->active;
      if (active) {
        sh->staffed[CASSA - 1]++;
        sh->active_workers++;
        if (!counted_worker) {
          sh->day_active++;
          sh->total_workers++;
          counted_worker = 1;
        }
      }
      unlock_shared(sid);
      if (!active) {
        sem_v(sid, station_sem);
        break;
      }
      holds_station = 1;
    }

    // Attesa se i pagamenti sono bloccati da communication_disorder
    while (simulation_is_active(sid, sh)) {
      lock_shared(sid);
      int blocked = sh->cassa_blocked;
      long long min_ns = sh->cfg.minute_ns;
      unlock_shared(sid);
      if (!blocked)
        break;
      struct timespec block_ts = {(time_t)(min_ns / 1000000000),
                                  (long)(min_ns % 1000000000)};
      nanosleep(&block_ts, NULL);
    }
    if (!simulation_is_active(sid, sh))
      break;

    Message req;
    if (msgrcv(mid, &req, sizeof req - sizeof(long), CASSA, 0) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    long long dequeued_ns = monotonic_time_ns();
    lock_shared(sid);
    if (sh->queue[CASSA - 1] > 0)
      sh->queue[CASSA - 1]--;
    unlock_shared(sid);
    if (!simulation_is_active(sid, sh))
      break;

    int avg = sh->cfg.service_ms[3], span = avg / 5,
        duration =
            avg - span + (span ? (int)(rand_r(&seed) % (2 * span + 1)) : 0);
    long ns = scale_ms(&sh->cfg, duration);
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    nanosleep(&ts, NULL);

    int group_sz = req.group_size > 0 ? req.group_size : 1;
    int dishes = req.dish;
    double cost = ((dishes & 1 ? sh->cfg.price[0] : 0) +
                   (dishes & 2 ? sh->cfg.price[1] : 0) +
                   (req.coffee ? sh->cfg.price[2] : 0)) *
                  group_sz;

    if (req.has_ticket) {
      cost = cost * (100.0 - sh->cfg.ticket_discount) / 100.0;
      long ticket_ns = scale_ms(&sh->cfg, 500);
      struct timespec ticket_ts = {ticket_ns / 1000000000,
                                   ticket_ns % 1000000000};
      nanosleep(&ticket_ts, NULL);
    }

    lock_shared(sid);
    if (!sh->active) {
      unlock_shared(sid);
      break;
    }
    sh->revenue += cost;
    sh->day_revenue += cost;
    sh->served += group_sz;
    sh->day_served += group_sz;
    unlock_shared(sid);

    Reply rep = {.type = req.user,
                 .ok = 1,
                 .dish = 0,
                 .queue_wait_ns = dequeued_ns > req.enqueued_ns
                                      ? dequeued_ns - req.enqueued_ns
                                      : 0};
    msgsnd(mid, &rep, sizeof rep - sizeof(long), 0);

    if (simulation_is_active(sid, sh) && pauses_taken < sh->cfg.pauses &&
        rand_r(&seed) % 100 < 8) {
      lock_shared(sid);
      int can_pause = sh->active && sh->staffed[CASSA - 1] > 1;
      if (can_pause) {
        sh->staffed[CASSA - 1]--;
        sh->active_workers--;
        holds_station = 0;
        sh->pauses++;
        sh->day_pauses++;
      }
      unlock_shared(sid);
      if (can_pause) {
        pauses_taken++;
        sem_v(sid, station_sem);
        int pause_ms = 1000 + (int)(rand_r(&seed) % 2000);
        long pause_ns = scale_ms(&sh->cfg, pause_ms);
        struct timespec pause_ts = {pause_ns / 1000000000,
                                    pause_ns % 1000000000};
        nanosleep(&pause_ts, NULL);
      }
    }
  }
  if (holds_station) {
    lock_shared(sid);
    sh->staffed[CASSA - 1]--;
    sh->active_workers--;
    unlock_shared(sid);
    sem_v(sid, station_sem);
  }
  shmdt(sh);
  return 0;
}