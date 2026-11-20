#include "common.h"

int main(void) {
  int sid = semget(ftok(FTOK_PATH, FTOK_SEM), NUM_SEMAFORI, 0666);
  int hid = shmget(ftok(FTOK_PATH, FTOK_SHM), sizeof(Shared), 0666);
  if (sid < 0 || hid < 0) {
    fprintf(stderr, "[Disorder] Simulazione non attiva o IPC non trovati.\n");
    return 1;
  }

  Shared *sh = shmat(hid, NULL, 0);
  if (sh == (void *)-1) {
    perror("shmat");
    return 1;
  }

  lock_shared(sid);
  if (!sh->active) {
    unlock_shared(sid);
    shmdt(sh);
    return 0;
  }
  sh->cassa_blocked = 1;
  int duration_mins = sh->cfg.stop_duration;
  long long minute_ns = sh->cfg.minute_ns;
  unlock_shared(sid);

  printf("[Disorder] Blocco pagamenti cassa attivato per %d minuti simulati!\n", duration_mins);

  long long total_ns = (long long)duration_mins * minute_ns;
  struct timespec ts = {total_ns / 1000000000, total_ns % 1000000000};
  nanosleep(&ts, NULL);

  lock_shared(sid);
  sh->cassa_blocked = 0;
  unlock_shared(sid);

  printf("[Disorder] Guasto risolto, pagamenti in cassa ripristinati.\n");
  shmdt(sh);
  return 0;
}