#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#define FTOK_PATH "."
#define FTOK_SHM 'S'
#define FTOK_SEM 'E'
#define FTOK_MSG 'Q'
#define MAX_DISHES 8
#define MENU_FILE "menu.txt"
enum { SEM_START, SEM_MUTEX, SEM_TABLES, SEM_GATE, NUM_SEMAFORI };
enum { PRIMI = 1, SECONDI, COFFEE, CASSA };
typedef struct {
    int sim_duration, users, workers, seats[4], tables;
    int max_portions[2], refill[2], service_ms[4], refill_minutes;
    int pauses, overload, day_minutes, minute_ns;
    int price[3], n_dishes[2], coffee_types, dessert_count;
    char dishes[2][MAX_DISHES][32];
    char coffees[4][32], desserts[MAX_DISHES][32];
} Config;
typedef struct {
    Config cfg;
    int active, portions[2][MAX_DISHES], queue[4], staffed[4];
    int served, unserved, distributed[3], leftovers[3], day_leftovers[3], active_workers, pauses;
    int day_served, day_unserved, day_distributed[3], day_active, day_pauses;
    long long wait_ms[4], wait_count[4], day_wait_ms[4], day_wait_count[4];
    double revenue, day_revenue;
} Shared;
typedef struct { long type; pid_t user; int station, dish, coffee, dessert; } Message;
typedef struct { long type; int ok, dish; } Reply;
void sem_p(int sem_id, int sem_num);
void sem_v(int sem_id, int sem_num);
void lock_shared(int sem_id);
void unlock_shared(int sem_id);
long scale_ms(const Config *cfg, int ms);
#endif
