#ifndef COMMON_H
#define COMMON_H

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FTOK_PATH "."
#define FTOK_SHM 'S'
#define FTOK_SEM 'E'
#define FTOK_MSG 'Q'
#define MAX_DISHES 8
#define MENU_FILE "menu.txt"
enum {
  SEM_START,
  SEM_MUTEX,
  SEM_TABLES,
  SEM_GATE,
  SEM_STATION_PRIMI,
  SEM_STATION_SECONDI,
  SEM_STATION_COFFEE,
  SEM_STATION_CASSA,
  NUM_SEMAFORI
};
enum { PRIMI = 1, SECONDI, COFFEE, CASSA };
typedef struct {
  int sim_duration, users, workers, seats[4], tables;
  int max_portions[2], refill[2], service_ms[4], refill_minutes;
  int pauses, overload, day_minutes, minute_ns;
  int price[3], n_dishes[2], coffee_types, dessert_count;
  char dishes[2][MAX_DISHES][32];
  char sides[MAX_DISHES][32];
  char coffees[4][32], desserts[MAX_DISHES][32];
} Config;
typedef struct {
  Config cfg;
  int active, portions[2][MAX_DISHES], queue[4], staffed[4];
  int served, unserved, distributed[3];
  int active_workers, total_workers, pauses;
  int day_served, day_unserved, day_distributed[3], day_active, day_pauses;
  double wait_minutes[4], day_wait_minutes[4];
  long long wait_count[4], day_wait_count[4];
  double revenue, day_revenue;
} Shared;
typedef struct {
  long type;
  pid_t user;
  int station, dish, coffee, dessert;
  long long enqueued_ns;
} Message;
typedef struct {
  long type;
  int ok, dish;
  long long queue_wait_ns;
} Reply;
void sem_p(int sem_id, int sem_num);
void sem_v(int sem_id, int sem_num);
void lock_shared(int sem_id);
void unlock_shared(int sem_id);
int simulation_is_active(int sem_id, Shared *shared);
long scale_ms(const Config *cfg, int ms);
long long monotonic_time_ns(void);
#endif
