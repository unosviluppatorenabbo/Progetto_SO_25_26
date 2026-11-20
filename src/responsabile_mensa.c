#include "common.h"
#include <sys/wait.h>

static int shm_id = -1, sem_id = -1, msg_id = -1;
static Shared *shared;
static FILE *stats_csv;
static pid_t children[512];
static int child_count;
static volatile sig_atomic_t requested_signal;

union semun
{
  int val;
  struct semid_ds *buf;
  unsigned short *array;
};

typedef struct
{
  int served, unserved, distributed[3], workers, pauses;
  double wait_minutes[4], revenue;
  long long wait_count[4];
} StatsSnapshot;

typedef struct
{
  const char *name;
  double value;
} CsvMetric;

static void handle_shutdown_signal(int signal) { requested_signal = signal; }

static const char *termination_signal_name(void)
{
  return requested_signal == SIGINT ? "SIGINT" : "SIGTERM";
}

static void cleanup(void)
{
  if (stats_csv)
  {
    if (fclose(stats_csv) == EOF)
      perror("closing output.csv");
    stats_csv = NULL;
  }
  if (shared && shared != (void *)-1)
  {
    shmdt(shared);
    shared = NULL;
  }
  if (shm_id >= 0)
  {
    shmctl(shm_id, IPC_RMID, NULL);
    shm_id = -1;
  }
  if (sem_id >= 0)
  {
    semctl(sem_id, 0, IPC_RMID);
    sem_id = -1;
  }
  if (msg_id >= 0)
  {
    msgctl(msg_id, IPC_RMID, NULL);
    msg_id = -1;
  }
}

static int wait_for_start(void)
{
  struct sembuf op = {SEM_START, -1, 0};
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};
  while (!requested_signal)
  {
    if (semtimedop(sem_id, &op, 1, &timeout) == 0)
      return 0;
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN)
      return -1;

    for (int i = 0; i < child_count; i++)
    {
      if (children[i] <= 0)
        continue;
      int status;
      pid_t result = waitpid(children[i], &status, WNOHANG);
      if (result == children[i])
      {
        children[i] = 0;
        errno = ECHILD;
        return -1;
      }
      if (result < 0 && errno == ECHILD)
      {
        children[i] = 0;
        return -1;
      }
      if (result < 0 && errno != EINTR)
        return -1;
    }
  }
  errno = EINTR;
  return -1;
}

static void stop_children(void)
{
  int extra_users = 0;
  if (shared && shared != (void *)-1)
  {
    lock_shared(sem_id);
    shared->active = 0;
    shared->cassa_blocked = 0;
    extra_users = shared->dynamic_users_added;
    shared->dynamic_users_added = 0;
    unlock_shared(sem_id);
  }
  for (int i = 0; i < child_count + extra_users; i++)
  {
    sem_v(sem_id, SEM_GATE);
    sem_v(sem_id, SEM_TABLES);
    for (int station = 0; station < 4; station++)
      sem_v(sem_id, SEM_STATION_PRIMI + station);
  }
  for (int i = 0; i < child_count; i++)
    if (children[i] > 0)
      kill(children[i], SIGTERM);
  for (int i = 0; i < child_count; i++)
  {
    if (children[i] <= 0)
      continue;
    while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR)
    {
    }
  }
  // Raccoglie eventuali processi utente aggiunti a runtime
  while (waitpid(-1, NULL, WNOHANG) > 0)
  {
  }
  child_count = 0;
  union semun arg = {.val = 0};
  semctl(sem_id, SEM_START, SETVAL, arg);
  semctl(sem_id, SEM_GATE, SETVAL, arg);
  if (shared && shared != (void *)-1)
  {
    arg.val = shared->cfg.tables;
    semctl(sem_id, SEM_TABLES, SETVAL, arg);
    for (int station = 0; station < 4; station++)
    {
      arg.val = shared->cfg.seats[station];
      semctl(sem_id, SEM_STATION_PRIMI + station, SETVAL, arg);
    }
  }
}

static void drain_messages(void)
{
  Message msg;
  while (1)
  {
    if (msgrcv(msg_id, &msg, sizeof msg - sizeof(long), 0, IPC_NOWAIT) >= 0)
      continue;
    if (errno == EINTR)
      continue;
    if (errno != ENOMSG)
      perror("msgrcv");
    break;
  }
}

static double average(double total, long long count)
{
  return count > 0 ? total / count : 0.0;
}

static int csv_write_group(const char *record_type, int day,
                           const char *termination, const CsvMetric *metrics,
                           size_t count, int add_day_separator)
{
  for (size_t i = 0; i < count; i++)
    if (fprintf(stats_csv, "%s,%d,%s,%s,%.10g\n", record_type, day, termination,
                metrics[i].name, metrics[i].value) < 0)
      return -1;
  if (add_day_separator && fprintf(stats_csv, "DAY_END,%d,,,\n\n", day) < 0)
    return -1;
  return fflush(stats_csv) == EOF ? -1 : 0;
}

static void capture_stats(StatsSnapshot *snapshot)
{
  lock_shared(sem_id);
  snapshot->served = shared->day_served;
  snapshot->unserved = shared->day_unserved;
  memcpy(snapshot->distributed, shared->day_distributed,
         sizeof snapshot->distributed);
  snapshot->workers = shared->day_active;
  snapshot->pauses = shared->day_pauses;
  memcpy(snapshot->wait_minutes, shared->day_wait_minutes,
         sizeof snapshot->wait_minutes);
  memcpy(snapshot->wait_count, shared->day_wait_count,
         sizeof snapshot->wait_count);
  snapshot->revenue = shared->day_revenue;
  unlock_shared(sem_id);
}

static int load_config(Config *c, const char *path)
{
  FILE *f = fopen(path, "r");
  char line[256];
  if (!f)
    return -1;
  while (fgets(line, sizeof line, f))
  {
    char key[80];
    int v;
    if (sscanf(line, "%79[^=]=%d", key, &v) != 2)
      continue;
    if (!strcmp(key, "SIM_DURATION"))
      c->sim_duration = v;
    else if (!strcmp(key, "NOF_USERS"))
      c->users = v;
    else if (!strcmp(key, "NOF_WORKERS"))
      c->workers = v;
    else if (!strcmp(key, "NOF_WK_SEATS_PRIMI"))
      c->seats[0] = v;
    else if (!strcmp(key, "NOF_WK_SEATS_SECONDI"))
      c->seats[1] = v;
    else if (!strcmp(key, "NOF_WK_SEATS_COFFEE"))
      c->seats[2] = v;
    else if (!strcmp(key, "NOF_WK_SEATS_CASSA"))
      c->seats[3] = v;
    else if (!strcmp(key, "NOF_TABLE_SEATS"))
      c->tables = v;
    else if (!strcmp(key, "MAX_PORZIONI_PRIMI"))
      c->max_portions[0] = v;
    else if (!strcmp(key, "MAX_PORZIONI_SECONDI"))
      c->max_portions[1] = v;
    else if (!strcmp(key, "AVG_REFILL_PRIMI"))
      c->refill[0] = v;
    else if (!strcmp(key, "AVG_REFILL_SECONDI"))
      c->refill[1] = v;
    else if (!strcmp(key, "AVG_SRVC_PRIMI"))
      c->service_ms[0] = v * 1000;
    else if (!strcmp(key, "AVG_SRVC_MAIN_COURSE"))
      c->service_ms[1] = v * 1000;
    else if (!strcmp(key, "AVG_SRVC_COFFEE"))
      c->service_ms[2] = v * 1000;
    else if (!strcmp(key, "AVG_SRVC_CASSA"))
      c->service_ms[3] = v * 1000;
    else if (!strcmp(key, "AVG_REFILL_TIME"))
      c->refill_minutes = v;
    else if (!strcmp(key, "NOF_PAUSE"))
      c->pauses = v;
    else if (!strcmp(key, "OVERLOAD_THRESHOLD"))
      c->overload = v;
    else if (!strcmp(key, "DAY_MINUTES"))
      c->day_minutes = v;
    else if (!strcmp(key, "N_NANO_SECS"))
      c->minute_ns = v;
    else if (!strcmp(key, "PRICE_PRIMI"))
      c->price[0] = v;
    else if (!strcmp(key, "PRICE_SECONDI"))
      c->price[1] = v;
    else if (!strcmp(key, "PRICE_COFFEE"))
      c->price[2] = v;
    else if (!strcmp(key, "STOP_DURATION"))
      c->stop_duration = v;
    else if (!strcmp(key, "TICKET_DISCOUNT"))
      c->ticket_discount = v;
    else if (!strcmp(key, "MAX_USERS_PER_GROUP"))
      c->max_users_per_group = v;
  }
  fclose(f);
  return c->sim_duration > 0 && c->users > 0 && c->workers >= 4 &&
                 c->workers + c->users <= 512 && c->day_minutes > 0 &&
                 c->minute_ns > 0 && c->seats[0] > 0 && c->seats[1] > 0 &&
                 c->seats[2] > 0 && c->seats[3] > 0 && c->tables > 0
             ? 0
             : -1;
}

static int load_menu(Config *c)
{
  FILE *f = fopen(MENU_FILE, "r");
  char line[128], name[96];
  int kind;
  if (!f)
    return -1;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "%d %95[^\n]", &kind, name) == 2)
    {
      if (kind == 0 && c->n_dishes[0] < MAX_DISHES &&
          strlen(name) < sizeof c->dishes[0][0])
      {
        strcpy(c->dishes[kind][c->n_dishes[kind]++], name);
      }
      else if (kind == 1 && c->n_dishes[1] < MAX_DISHES)
      {
        char *separator = strchr(name, '|');
        if (!separator)
          continue;
        *separator = '\0';
        char *second = name;
        char *side = separator + 1;
        while (*second == ' ' || *second == '\t')
          second++;
        while (*side == ' ' || *side == '\t')
          side++;
        size_t second_len = strlen(second);
        size_t side_len = strlen(side);
        while (second_len > 0 && (second[second_len - 1] == ' ' ||
                                  second[second_len - 1] == '\t'))
          second[--second_len] = '\0';
        while (side_len > 0 &&
               (side[side_len - 1] == ' ' || side[side_len - 1] == '\t' ||
                side[side_len - 1] == '\r' || side[side_len - 1] == '\n'))
          side[--side_len] = '\0';
        if (second_len == 0 || side_len == 0 ||
            second_len >= sizeof c->dishes[1][0] ||
            side_len >= sizeof c->sides[0])
          continue;
        int option = c->n_dishes[1]++;
        strcpy(c->dishes[1][option], second);
        strcpy(c->sides[option], side);
      }
      else if (kind == 2 && c->coffee_types < 4 &&
               strlen(name) < sizeof c->coffees[0])
        strcpy(c->coffees[c->coffee_types++], name);
      else if (kind == 3 && c->dessert_count < MAX_DISHES &&
               strlen(name) < sizeof c->desserts[0])
        strcpy(c->desserts[c->dessert_count++], name);
    }
  fclose(f);
  return c->n_dishes[0] >= 2 && c->n_dishes[1] >= 2 && c->coffee_types >= 4 &&
                 c->dessert_count >= 1
             ? 0
             : -1;
}

static void launch(const char *exe, const char *arg)
{
  pid_t p = fork();
  if (p == 0)
  {
    char *args[] = {(char *)exe, (char *)arg, NULL};
    extern char **environ;
    execve(exe, args, environ);
    perror("execve");
    _exit(127);
  }
  if (p < 0 || child_count >= 512)
  {
    perror("fork");
    exit(EXIT_FAILURE);
  }
  children[child_count++] = p;
}

// Calcola l'intervallo casuale di refill +-20% intorno ad AVG_REFILL_TIME
static int next_refill_interval(int avg_refill_minutes, unsigned int *seed)
{
  if (avg_refill_minutes <= 1)
    return 1;
  int span = avg_refill_minutes * 20 / 100;

  if (span == 0)
    span = 1;
  int delta = (int)(rand_r(seed) % (2 * span + 1)) - span;
  int val = avg_refill_minutes + delta;
  return val < 1 ? 1 : val;
}
static void print_stats(int day, int days, const StatsSnapshot *before,
                        int total_leftovers[3])
{
  lock_shared(sem_id);
  int day_served = shared->day_served - before->served;
  int day_unserved = shared->day_unserved - before->unserved;
  int day_distributed[3];
  for (int k = 0; k < 3; k++)
    day_distributed[k] = shared->day_distributed[k] - before->distributed[k];
  int day_dishes = day_distributed[0] + day_distributed[1] + day_distributed[2];
  int day_active = shared->day_active - before->workers;
  int day_pauses = shared->day_pauses - before->pauses;
  double day_revenue = shared->day_revenue - before->revenue;
  int total =
      shared->distributed[0] + shared->distributed[1] + shared->distributed[2];
  int day_leftovers[3] = {0};
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < shared->cfg.n_dishes[k]; j++)
      day_leftovers[k] += shared->portions[k][j];
  for (int k = 0; k < 3; k++)
    total_leftovers[k] += day_leftovers[k];
  printf("\nDay %d statistics:\n served=%d unserved=%d\n dishes=%d [first=%d "
         "second=%d coffee/dessert=%d]\n leftovers=%d\n workers_active=%d \n"
         "\033[33mpauses=%d\033[0m \nrevenue=%.2f\n",
         day, day_served, day_unserved, day_dishes, day_distributed[0],
         day_distributed[1], day_distributed[2],
         day_leftovers[0] + day_leftovers[1], day_active, day_pauses,
         day_revenue);
  printf(
      "Leftovers by type:\n first=%d second=%d coffee/dessert=%d;\n "
      "averages/day:\n served=%.2f unserved=%.2f\n dishes=%.2f [first=%.2f "
      "second=%.2f coffee/dessert=%.2f]\n leftovers/day=%.2f [first=%.2f "
      "second=%.2f coffee/dessert=%.2f]\n",
      day_leftovers[0], day_leftovers[1], day_leftovers[2],
      (double)shared->served / day, (double)shared->unserved / day,
      (double)total / day, (double)shared->distributed[0] / day,
      (double)shared->distributed[1] / day,
      (double)shared->distributed[2] / day,
      (double)(total_leftovers[0] + total_leftovers[1] + total_leftovers[2]) /
          day,
      (double)total_leftovers[0] / day, (double)total_leftovers[1] / day,
      (double)total_leftovers[2] / day);
  double day_wait_minutes[4];
  long long day_wait_count[4];
  long long total_day_wait_count = 0;
  double total_day_wait_minutes = 0;
  for (int i = 0; i < 4; i++)
  {
    day_wait_minutes[i] = shared->day_wait_minutes[i] - before->wait_minutes[i];
    day_wait_count[i] = shared->day_wait_count[i] - before->wait_count[i];
    total_day_wait_minutes += day_wait_minutes[i];
    total_day_wait_count += day_wait_count[i];
  }
  if (stats_csv)
  {
    CsvMetric metrics[] = {
        {"served", day_served},
        {"unserved", day_unserved},
        {"dishes_total", day_dishes},
        {"first_distributed", day_distributed[0]},
        {"second_distributed", day_distributed[1]},
        {"coffee_dessert_distributed", day_distributed[2]},
        {"leftovers_total",
         day_leftovers[0] + day_leftovers[1] + day_leftovers[2]},
        {"leftovers_first", day_leftovers[0]},
        {"leftovers_second", day_leftovers[1]},
        {"leftovers_coffee_dessert", day_leftovers[2]},
        {"served_avg_per_day", (double)shared->served / day},
        {"unserved_avg_per_day", (double)shared->unserved / day},
        {"dishes_avg_per_day", (double)total / day},
        {"first_dishes_avg_per_day", (double)shared->distributed[0] / day},
        {"second_dishes_avg_per_day", (double)shared->distributed[1] / day},
        {"coffee_dessert_avg_per_day", (double)shared->distributed[2] / day},
        {"leftovers_avg_per_day",
         (double)(total_leftovers[0] + total_leftovers[1] +
                  total_leftovers[2]) /
             day},
        {"leftovers_first_avg_per_day", (double)total_leftovers[0] / day},
        {"leftovers_second_avg_per_day", (double)total_leftovers[1] / day},
        {"leftovers_coffee_dessert_avg_per_day",
         (double)total_leftovers[2] / day},
        {"wait_avg_sim_minutes",
         average(total_day_wait_minutes, total_day_wait_count)},
        {"wait_first_sim_minutes",
         average(day_wait_minutes[0], day_wait_count[0])},
        {"wait_second_sim_minutes",
         average(day_wait_minutes[1], day_wait_count[1])},
        {"wait_coffee_sim_minutes",
         average(day_wait_minutes[2], day_wait_count[2])},
        {"wait_cashier_sim_minutes",
         average(day_wait_minutes[3], day_wait_count[3])},
        {"workers_active", day_active},
        {"pauses", day_pauses},
        {"avg_pauses_per_worker",
         (double)day_pauses / (day_active ? day_active : 1)},
        {"revenue", day_revenue},
        {"revenue_avg_per_day", (double)shared->revenue / day}};
    if (csv_write_group("DAY", day, "", metrics,
                        sizeof metrics / sizeof metrics[0], 1) < 0)
      perror("writing output.csv");
  }
  printf(
      "Wait avg simulated minutes:\n all %.4f,\n first %.4f,\n second %.4f,\n "
      "coffee %.4f,\n cashier %.4f\n",
      average(total_day_wait_minutes, total_day_wait_count),
      average(day_wait_minutes[0], day_wait_count[0]),
      average(day_wait_minutes[1], day_wait_count[1]),
      average(day_wait_minutes[2], day_wait_count[2]),
      average(day_wait_minutes[3], day_wait_count[3]));
  printf("Average worker pauses today: \033[33m%.2f\033[0m\n",
         (double)day_pauses / (day_active ? day_active : 1));
  if (day == days)
    printf("Totals:\n served=%d unserved=%d dishes=%d [first=%d second=%d "
           "coffee/dessert=%d],\n leftovers [first=%d second=%d "
           "coffee/dessert=%d],\n "
           "workers_active=%d \033[33mpauses=%d\033[0m revenue=%.2f "
           "avg/day=%.2f\n",
           shared->served, shared->unserved, total, shared->distributed[0],
           shared->distributed[1], shared->distributed[2], total_leftovers[0],
           total_leftovers[1], total_leftovers[2], shared->total_workers,
           shared->pauses, shared->revenue, shared->revenue / days);
  unlock_shared(sem_id);
}
static void report_interrupted_day(int day, const StatsSnapshot *day_start,
                                   int total_leftovers[3])
{
  stop_children();
  drain_messages();
  print_stats(day, 0, day_start, total_leftovers);
}
int main(int argc, char **argv)
{
  struct sigaction action = {.sa_handler = handle_shutdown_signal};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) < 0 ||
      sigaction(SIGTERM, &action, NULL) < 0)
  {
    perror("sigaction");
    return 1;
  }
  unsigned int seed = (unsigned int)getpid() ^ (unsigned int)time(NULL);
  Config cfg = {.sim_duration = 3,
                .users = 15,
                .workers = 6,
                .seats = {4, 4, 3, 2},
                .tables = 12,
                .max_portions = {30, 25},
                .refill = {10, 10},
                .service_ms = {1000, 1500, 1000, 500},
                .refill_minutes = 10,
                .pauses = 2,
                .overload = 20,
                .day_minutes = 60,
                .minute_ns = 10000000,
                .price = {5, 7, 1},
                .stop_duration = 5,
                .ticket_discount = 20,
                .max_users_per_group = 4};
  const char *conf = argc > 1 ? argv[1] : "config.txt";
  if (load_config(&cfg, conf) < 0 || load_menu(&cfg) < 0)
  {
    fprintf(stderr, "Invalid config or menu (check worker/station capacities "
                    "and menu.txt).\n");
    return 1;
  }
  stats_csv = fopen("output.csv", "w");
  if (!stats_csv)
  {
    perror("output.csv");
    return 1;
  }
  if (fputs("record_type,day,termination_cause,metric,value\n", stats_csv) ==
      EOF)
  {
    perror("writing output.csv header");
    cleanup();
    return 1;
  }
  shm_id = shmget(ftok(FTOK_PATH, FTOK_SHM), sizeof(Shared), IPC_CREAT | 0666);
  if (shm_id < 0)
  {
    perror("shmget");
    return 1;
  }
  shared = shmat(shm_id, NULL, 0);
  if (shared == (void *)-1)
  {
    perror("shmat");
    shared = NULL;
    cleanup();
    return 1;
  }
  sem_id = semget(ftok(FTOK_PATH, FTOK_SEM), NUM_SEMAFORI, IPC_CREAT | 0666);
  msg_id = msgget(ftok(FTOK_PATH, FTOK_MSG), IPC_CREAT | 0666);
  if (sem_id < 0 || msg_id < 0)
  {
    perror("IPC create");
    cleanup();
    return 1;
  }
  memset(shared, 0, sizeof *shared);
  shared->cfg = cfg;
  unsigned short values[NUM_SEMAFORI] = {0,
                                         1,
                                         (unsigned short)cfg.tables,
                                         0,
                                         (unsigned short)cfg.seats[0],
                                         (unsigned short)cfg.seats[1],
                                         (unsigned short)cfg.seats[2],
                                         (unsigned short)cfg.seats[3]};
  union semun arg = {.array = values};
  if (semctl(sem_id, 0, SETALL, arg) < 0)
  {
    perror("semctl SETALL");
    cleanup();
    return 1;
  }
  const char *termination = "timeout";
  int completed_days = 0;
  int total_leftovers[3] = {0};
  for (int day = 1; day <= cfg.sim_duration && !requested_signal; day++)
  {
    arg.val = cfg.tables;
    if (semctl(sem_id, SEM_TABLES, SETVAL, arg) < 0)
    {
      perror("semctl tables");
      cleanup();
      return 1;
    }
    lock_shared(sem_id);
    shared->active = 1;
    shared->cassa_blocked = 0;
    shared->dynamic_users_added = 0;
    memset(shared->queue, 0, sizeof shared->queue);
    memset(shared->staffed, 0, sizeof shared->staffed);
    for (int k = 0; k < 2; k++)
      for (int d = 0; d < cfg.n_dishes[k]; d++)
        shared->portions[k][d] = cfg.refill[k];
    unlock_shared(sem_id);
    StatsSnapshot day_start;
    capture_stats(&day_start);
    int allocation[4] = {1, 1, 1, 1};
    int left = cfg.workers - 4;
    while (left-- > 0)
    {
      int best = -1;
      for (int s = 0; s < 4; s++)
        if (best < 0 || (double)cfg.service_ms[s] / allocation[s] >
                            (double)cfg.service_ms[best] / allocation[best])
          best = s;
      if (best < 0)
        break;
      allocation[best]++;
    }
    int started = 0;
    for (int s = 0; s < 4; s++)
      for (int n = 0; n < allocation[s]; n++)
      {
        char a[8];
        snprintf(a, sizeof a, "%d", s + 1);
        launch(s == 3 ? "./bin/operatore_cassa" : "./bin/operatore",
               s == 3 ? NULL : a);
        started++;
      }
    for (int u = 0; u < cfg.users; u++)
      launch("./bin/utente", "0");
    started += cfg.users;
    int ready = 1;
    for (int n = 0; n < started; n++)
    {
      if (wait_for_start() < 0)
      {
        ready = 0;
        break;
      }
    }
    if (!ready)
    {
      if (!requested_signal)
      {
        perror("waiting for child startup");
        termination = "error";
        stop_children();
        drain_messages();
      }
      else
      {
        termination = termination_signal_name();
        report_interrupted_day(day, &day_start, total_leftovers);
        completed_days++;
      }
      break;
    }
    for (int n = 0; n < started; n++)
      sem_v(sem_id, SEM_GATE);
    if (requested_signal)
    {
      termination = termination_signal_name();
      report_interrupted_day(day, &day_start, total_leftovers);
      completed_days++;
      break;
    }
    printf("Day %d/%d opened (%d workers, %d users).\n", day, cfg.sim_duration,
           cfg.workers, cfg.users);
    int overload = 0;
    int next_refill = next_refill_interval(cfg.refill_minutes, &seed);
    for (int minute = 0; minute < cfg.day_minutes; minute++)
    {
      if (requested_signal)
        break;
      long long ns = cfg.minute_ns;
      struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
      nanosleep(&t, NULL);
      if (requested_signal)
        break;
      if (minute > 0 && minute >= next_refill)
      {
        lock_shared(sem_id);
        for (int k = 0; k < 2; k++)
          for (int d = 0; d < cfg.n_dishes[k]; d++)
          {
            shared->portions[k][d] += cfg.refill[k];
            if (shared->portions[k][d] > cfg.max_portions[k])
              shared->portions[k][d] = cfg.max_portions[k];
          }
        unlock_shared(sem_id);
        next_refill = minute + next_refill_interval(cfg.refill_minutes, &seed);
      }
    }
    if (requested_signal)
    {
      termination = termination_signal_name();
      report_interrupted_day(day, &day_start, total_leftovers);
      completed_days++;
      break;
    }
    lock_shared(sem_id);
    shared->active = 0;
    overload = shared->queue[0] + shared->queue[1] + shared->queue[2] +
                   shared->queue[3] >
               cfg.overload;
    unlock_shared(sem_id);
    stop_children();
    drain_messages();
    print_stats(day, cfg.sim_duration, &day_start, total_leftovers);
    completed_days++;
    if (overload)
    {
      termination = "overload";
      break;
    }
  }
  if (requested_signal)
    termination = termination_signal_name();
  printf("\nFinal termination cause: \033[31m%s\033[0m.\n", termination);
  lock_shared(sem_id);
  long long total_count = 0;
  double total_wait = 0;
  for (int i = 0; i < 4; i++)
  {
    total_wait += shared->wait_minutes[i];
    total_count += shared->wait_count[i];
  }
  int total_dishes =
      shared->distributed[0] + shared->distributed[1] + shared->distributed[2];
  int total_leftover_count =
      total_leftovers[0] + total_leftovers[1] + total_leftovers[2];
  int average_days = completed_days ? completed_days : 1;
  if (stats_csv)
  {
    CsvMetric metrics[] = {
        {"served", shared->served},
        {"unserved", shared->unserved},
        {"dishes_total", total_dishes},
        {"first_distributed", shared->distributed[0]},
        {"second_distributed", shared->distributed[1]},
        {"coffee_dessert_distributed", shared->distributed[2]},
        {"leftovers_total", total_leftover_count},
        {"leftovers_first", total_leftovers[0]},
        {"leftovers_second", total_leftovers[1]},
        {"leftovers_coffee_dessert", total_leftovers[2]},
        {"served_avg_per_day", (double)shared->served / average_days},
        {"unserved_avg_per_day", (double)shared->unserved / average_days},
        {"dishes_avg_per_day", (double)total_dishes / average_days},
        {"first_dishes_avg_per_day",
         (double)shared->distributed[0] / average_days},
        {"second_dishes_avg_per_day",
         (double)shared->distributed[1] / average_days},
        {"coffee_dessert_avg_per_day",
         (double)shared->distributed[2] / average_days},
        {"leftovers_avg_per_day", (double)total_leftover_count / average_days},
        {"leftovers_first_avg_per_day",
         (double)total_leftovers[0] / average_days},
        {"leftovers_second_avg_per_day",
         (double)total_leftovers[1] / average_days},
        {"leftovers_coffee_dessert_avg_per_day",
         (double)total_leftovers[2] / average_days},
        {"wait_avg_sim_minutes", average(total_wait, total_count)},
        {"wait_first_sim_minutes",
         average(shared->wait_minutes[0], shared->wait_count[0])},
        {"wait_second_sim_minutes",
         average(shared->wait_minutes[1], shared->wait_count[1])},
        {"wait_coffee_sim_minutes",
         average(shared->wait_minutes[2], shared->wait_count[2])},
        {"wait_cashier_sim_minutes",
         average(shared->wait_minutes[3], shared->wait_count[3])},
        {"workers_active", shared->total_workers},
        {"pauses", shared->pauses},
        {"avg_pauses_per_worker",
         average(shared->pauses, shared->total_workers)},
        {"revenue", shared->revenue},
        {"revenue_avg_per_day", shared->revenue / average_days}};
    if (csv_write_group("TOTAL", completed_days, termination, metrics,
                        sizeof metrics / sizeof metrics[0], 0) < 0)
      perror("writing output.csv totals");
  }
  printf("Simulation wait averages (simulated minutes):\n all %.4f;\n first "
         "%.4f;\n "
         "second %.4f;\n coffee %.4f;\n cashier %.4f\n",
         average(total_wait, total_count),
         average(shared->wait_minutes[0], shared->wait_count[0]),
         average(shared->wait_minutes[1], shared->wait_count[1]),
         average(shared->wait_minutes[2], shared->wait_count[2]),
         average(shared->wait_minutes[3], shared->wait_count[3]));
  printf(
      "Simulation totals:\n served=%d unserved=%d\n dishes [first=%d second=%d "
      "coffee/dessert=%d]\n leftovers [first=%d second=%d coffee/dessert=%d] \n"
      "workers_active=%d\n pauses=\033[33m%d\033[0m\n revenue=%.2f;\n avg "
      "revenue/day=%.2f\n",
      shared->served, shared->unserved, shared->distributed[0],
      shared->distributed[1], shared->distributed[2], total_leftovers[0],
      total_leftovers[1], total_leftovers[2], shared->total_workers,
      shared->pauses, shared->revenue,
      shared->revenue / (completed_days ? completed_days : 1));
  unlock_shared(sem_id);
  cleanup();
  return 0;
}