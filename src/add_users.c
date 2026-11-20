#include "common.h"

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Uso: %s <N_NEW_USERS>\n", argv[0]);
    return 1;
  }
  int n_new = atoi(argv[1]);
  if (n_new <= 0) return 1;

  int sid = semget(ftok(FTOK_PATH, FTOK_SEM), NUM_SEMAFORI, 0666);
  int hid = shmget(ftok(FTOK_PATH, FTOK_SHM), sizeof(Shared), 0666);
  if (sid < 0 || hid < 0) return 1;

  Shared *sh = shmat(hid, NULL, 0);
  if (sh == (void *)-1) return 1;

  lock_shared(sid);
  if (!sh->active) {
    unlock_shared(sid);
    shmdt(sh);
    return 0;
  }
  sh->dynamic_users_added += n_new;
  unlock_shared(sid);

  printf("[AddUsers] Generazione di %d nuovi utenti...\n", n_new);
  for (int i = 0; i < n_new; i++) {
    pid_t p = fork();
    if (p == 0) {
      // Passa "1" come argomento per indicare all'utente che è dinamico (salta SEM_GATE)
      char *args[] = {"./bin/utente", "1", NULL};
      extern char **environ;
      execve("./bin/utente", args, environ);
      perror("execve utente");
      _exit(127);
    }
  }

  shmdt(sh);
  return 0;
}
