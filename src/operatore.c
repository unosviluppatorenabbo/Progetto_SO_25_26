#include "common.h"

static void handle_stop_signal(int signal) { (void)signal; }

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  int station = atoi(argv[1]);
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
  int index = station - 1;
  int station_sem = SEM_STATION_PRIMI + index;
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
        sh->staffed[index]++;
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
    Message req;
    if (msgrcv(mid, &req, sizeof req - sizeof(long), station, 0) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    long long dequeued_ns = monotonic_time_ns();
    lock_shared(sid);
    if (sh->queue[index] > 0)
      sh->queue[index]--;
    unlock_shared(sid);
    if (!simulation_is_active(sid, sh))
      break;
    int avg = sh->cfg.service_ms[index];
    int percent = station == COFFEE ? 80 : station == CASSA ? 20 : 50;
    int span = avg * percent / 100;
    int duration = avg - span + (span ? rand_r(&seed) % (2 * span + 1) : 0);
    struct timespec ts = {scale_ms(&sh->cfg, duration) / 1000000000,
                          scale_ms(&sh->cfg, duration) % 1000000000};
    nanosleep(&ts, NULL);
    int ok = 1, dish = req.dish;
    lock_shared(sid);
    if (!sh->active) {
      unlock_shared(sid);
      break;
    }
    if (station == PRIMI || station == SECONDI) {
      int k = station - 1;
      if (dish < 0 || dish >= sh->cfg.n_dishes[k] ||
          sh->portions[k][dish] <= 0) {
        ok = 0;
        for (int j = 0; j < sh->cfg.n_dishes[k]; j++)
          if (sh->portions[k][j] > 0) {
            dish = j;
            ok = 1;
            break;
          }
      }
      if (ok) {
        sh->portions[k][dish]--;
        sh->distributed[k]++;
        sh->day_distributed[k]++;
      }
    } else {
      sh->distributed[2]++;
      sh->day_distributed[2]++;
    }
    unlock_shared(sid);
    Reply rep = {.type = req.user,
                 .ok = ok,
                 .dish = dish,
                 .queue_wait_ns = dequeued_ns > req.enqueued_ns
                                      ? dequeued_ns - req.enqueued_ns
                                      : 0};
    msgsnd(mid, &rep, sizeof rep - sizeof(long), 0);
    int pause_limit = sh->cfg.pauses;
    if (simulation_is_active(sid, sh) && pauses_taken < pause_limit &&
        rand_r(&seed) % 100 < 8) {
      lock_shared(sid);
      int can_pause = sh->active && sh->staffed[index] > 1;
      if (can_pause) {
        sh->staffed[index]--;
        sh->active_workers--;
        holds_station = 0;
        sh->pauses++;
        sh->day_pauses++;
      }
      unlock_shared(sid);
      if (can_pause) {
        pauses_taken++;
        sem_v(sid, station_sem);
        int ms = 1000 + (int)(rand_r(&seed) % 2000);
        long ns = scale_ms(&sh->cfg, ms);
        struct timespec pause_ts = {ns / 1000000000, ns % 1000000000};
        nanosleep(&pause_ts, NULL);
      }
    }
  }
  if (holds_station) {
    lock_shared(sid);
    sh->staffed[index]--;
    sh->active_workers--;
    unlock_shared(sid);
    sem_v(sid, station_sem);
  }
  shmdt(sh);
  return 0;
}
