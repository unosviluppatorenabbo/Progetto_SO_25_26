#include "common.h"
#include <sys/wait.h>

static int shm_id, sem_id, msg_id;
static Shared *shared;
static pid_t children[512];
static int child_count;

static void cleanup(void) {
    if (shared && shared != (void *)-1) shmdt(shared);
    if (shm_id >= 0) shmctl(shm_id, IPC_RMID, NULL);
    if (sem_id >= 0) semctl(sem_id, 0, IPC_RMID);
    if (msg_id >= 0) msgctl(msg_id, IPC_RMID, NULL);
}
static void stop_children(void) {
    shared->active = 0;
    for (int i = 0; i < child_count; i++) kill(children[i], SIGTERM);
    for (int i = 0; i < child_count; i++) waitpid(children[i], NULL, 0);
    child_count = 0;
}
static int load_config(Config *c, const char *path) {
    FILE *f = fopen(path, "r"); char line[256];
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char key[80]; int v;
        if (sscanf(line, "%79[^=]=%d", key, &v) != 2) continue;
        if (!strcmp(key,"SIM_DURATION")) c->sim_duration=v;
        else if (!strcmp(key,"NOF_USERS")) c->users=v;
        else if (!strcmp(key,"NOF_WORKERS")) c->workers=v;
        else if (!strcmp(key,"NOF_WK_SEATS_PRIMI")) c->seats[0]=v;
        else if (!strcmp(key,"NOF_WK_SEATS_SECONDI")) c->seats[1]=v;
        else if (!strcmp(key,"NOF_WK_SEATS_COFFEE")) c->seats[2]=v;
        else if (!strcmp(key,"NOF_WK_SEATS_CASSA")) c->seats[3]=v;
        else if (!strcmp(key,"NOF_TABLE_SEATS")) c->tables=v;
        else if (!strcmp(key,"MAX_PORZIONI_PRIMI")) c->max_portions[0]=v;
        else if (!strcmp(key,"MAX_PORZIONI_SECONDI")) c->max_portions[1]=v;
        else if (!strcmp(key,"AVG_REFILL_PRIMI")) c->refill[0]=v;
        else if (!strcmp(key,"AVG_REFILL_SECONDI")) c->refill[1]=v;
        else if (!strcmp(key,"AVG_SRVC_PRIMI")) c->service_ms[0]=v*1000;
        else if (!strcmp(key,"AVG_SRVC_MAIN_COURSE")) c->service_ms[1]=v*1000;
        else if (!strcmp(key,"AVG_SRVC_COFFEE")) c->service_ms[2]=v*1000;
        else if (!strcmp(key,"AVG_SRVC_CASSA")) c->service_ms[3]=v*1000;
        else if (!strcmp(key,"AVG_REFILL_TIME")) c->refill_minutes=v;
        else if (!strcmp(key,"NOF_PAUSE")) c->pauses=v;
        else if (!strcmp(key,"OVERLOAD_THRESHOLD")) c->overload=v;
        else if (!strcmp(key,"DAY_MINUTES")) c->day_minutes=v;
        else if (!strcmp(key,"N_NANO_SECS")) c->minute_ns=v;
        else if (!strcmp(key,"PRICE_PRIMI")) c->price[0]=v;
        else if (!strcmp(key,"PRICE_SECONDI")) c->price[1]=v;
        else if (!strcmp(key,"PRICE_COFFEE")) c->price[2]=v;
    }
    fclose(f);
    return c->sim_duration > 0 && c->users > 0 && c->workers >= 4 &&
        c->seats[0] > 0 && c->seats[1] > 0 && c->seats[2] > 0 && c->seats[3] > 0 && c->tables > 0 ? 0 : -1;
}
static int load_menu(Config *c) {
    FILE *f=fopen(MENU_FILE,"r"); char line[128], name[32]; int kind;
    if (!f) return -1;
    while (fgets(line,sizeof line,f)) if (sscanf(line,"%d %31[^\n]",&kind,name)==2) {
        if((kind==0||kind==1) && c->n_dishes[kind]<MAX_DISHES) strcpy(c->dishes[kind][c->n_dishes[kind]++],name);
        else if(kind==2 && c->coffee_types<4) strcpy(c->coffees[c->coffee_types++],name);
        else if(kind==3 && c->dessert_count<MAX_DISHES) strcpy(c->desserts[c->dessert_count++],name);
    }
    fclose(f);
    return c->n_dishes[0]>=2 && c->n_dishes[1]>=2 && c->coffee_types>=4 && c->dessert_count>=1 ? 0 : -1;
}
static void launch(const char *exe, const char *arg) {
    pid_t p=fork();
    if (p==0) { execl(exe,exe,arg,(char *)NULL); perror("exec"); _exit(127); }
    if (p<0 || child_count>=512) { perror("fork"); exit(EXIT_FAILURE); }
    children[child_count++]=p;
}
static void print_stats(int day, int days) {
    lock_shared(sem_id);
    int dt=shared->day_distributed[0]+shared->day_distributed[1]+shared->day_distributed[2];
    int total=shared->distributed[0]+shared->distributed[1]+shared->distributed[2];
    memset(shared->day_leftovers,0,sizeof shared->day_leftovers);
    for(int k=0;k<2;k++) for(int j=0;j<shared->cfg.n_dishes[k];j++) shared->day_leftovers[k]+=shared->portions[k][j];
    for(int k=0;k<3;k++) shared->leftovers[k]+=shared->day_leftovers[k];
    shared->day_unserved += shared->queue[0]+shared->queue[1]+shared->queue[2]+shared->queue[3];
    shared->unserved += shared->queue[0]+shared->queue[1]+shared->queue[2]+shared->queue[3];
    printf("\nDay %d statistics: served=%d unserved=%d dishes=%d [first=%d second=%d coffee/dessert=%d] leftovers=%d active_workers=%d pauses=%d revenue=%.2f\n",
      day,shared->day_served,shared->day_unserved,dt,shared->day_distributed[0],shared->day_distributed[1],shared->day_distributed[2],shared->day_leftovers[0]+shared->day_leftovers[1],shared->day_active,shared->day_pauses,shared->day_revenue);
    printf("Leftovers by type: first=%d second=%d coffee/dessert=%d; averages/day: served=%.2f unserved=%.2f dishes=%.2f [first=%.2f second=%.2f coffee/dessert=%.2f] leftovers=%.2f\n",
      shared->day_leftovers[0],shared->day_leftovers[1],shared->day_leftovers[2],(double)shared->served/day,(double)shared->unserved/day,(double)total/day,(double)shared->distributed[0]/day,(double)shared->distributed[1]/day,(double)shared->distributed[2]/day,(double)(shared->leftovers[0]+shared->leftovers[1])/day);
    printf("Wait avg ms: all %.2f, first %.2f, second %.2f, coffee %.2f, cashier %.2f\n",
      (double)(shared->day_wait_ms[0]+shared->day_wait_ms[1]+shared->day_wait_ms[2]+shared->day_wait_ms[3])/(shared->day_wait_count[0]+shared->day_wait_count[1]+shared->day_wait_count[2]+shared->day_wait_count[3]+1),
      (double)shared->day_wait_ms[0]/(shared->day_wait_count[0]+1),(double)shared->day_wait_ms[1]/(shared->day_wait_count[1]+1),(double)shared->day_wait_ms[2]/(shared->day_wait_count[2]+1),(double)shared->day_wait_ms[3]/(shared->day_wait_count[3]+1));
    printf("Average worker pauses today: %.2f\n",(double)shared->day_pauses/(shared->day_active?shared->day_active:1));
    shared->active_workers += shared->day_active; shared->pauses += shared->day_pauses;
    if(day==days) printf("Totals: served=%d unserved=%d dishes=%d [first=%d second=%d coffee/dessert=%d], leftovers [first=%d second=%d coffee/dessert=%d], active workers=%d pauses=%d revenue=%.2f avg/day=%.2f\n",
       shared->served,shared->unserved,total,shared->distributed[0],shared->distributed[1],shared->distributed[2],shared->leftovers[0],shared->leftovers[1],shared->leftovers[2],shared->active_workers,shared->pauses,shared->revenue,shared->revenue/days);
    unlock_shared(sem_id);
}
int main(int argc,char **argv) {
    Config cfg={.sim_duration=3,.users=15,.workers=6,.seats={4,4,3,2},.tables=12,.max_portions={30,25},.refill={10,10},.service_ms={1000,1500,1000,500},.refill_minutes=10,.pauses=2,.overload=20,.day_minutes=60,.minute_ns=10000000,.price={5,7,1}};
    const char *conf=argc>1?argv[1]:"config.txt";
    if(load_config(&cfg,conf)<0 || load_menu(&cfg)<0 || cfg.workers>cfg.seats[0]+cfg.seats[1]+cfg.seats[2]+cfg.seats[3]){fprintf(stderr,"Invalid config or menu (check worker/station capacities and menu.txt).\n");return 1;}
    shm_id=shmget(ftok(FTOK_PATH,FTOK_SHM),sizeof(Shared),IPC_CREAT|0666); if(shm_id<0){perror("shmget");return 1;}
    shared=shmat(shm_id,NULL,0); if(shared==(void *)-1){perror("shmat");return 1;}
    sem_id=semget(ftok(FTOK_PATH,FTOK_SEM),NUM_SEMAFORI,IPC_CREAT|0666); msg_id=msgget(ftok(FTOK_PATH,FTOK_MSG),IPC_CREAT|0666);
    if(sem_id<0||msg_id<0){perror("IPC create");cleanup();return 1;}
    memset(shared,0,sizeof *shared); shared->cfg=cfg;
    unsigned short values[NUM_SEMAFORI]={0,1,(unsigned short)cfg.tables,0};
    union semun {int val; struct semid_ds *buf; unsigned short *array;} arg={.array=values};
    semctl(sem_id,0,SETALL,arg);
    const char *termination="timeout";
    int completed_days=0;
    for(int day=1;day<=cfg.sim_duration;day++){
        lock_shared(sem_id); shared->active=1; shared->day_served=shared->day_unserved=shared->day_revenue=0; shared->day_active=shared->day_pauses=0; memset(shared->day_distributed,0,sizeof shared->day_distributed); memset(shared->day_wait_ms,0,sizeof shared->day_wait_ms); memset(shared->day_wait_count,0,sizeof shared->day_wait_count); memset(shared->queue,0,sizeof shared->queue);
        for(int k=0;k<2;k++) for(int d=0;d<cfg.n_dishes[k];d++) shared->portions[k][d]=cfg.refill[k];
        unlock_shared(sem_id);
        int allocation[4]={1,1,1,1}; int left=cfg.workers-4;
        while(left-->0){int best=-1;for(int s=0;s<4;s++)if(allocation[s]<cfg.seats[s]&&(best<0||(double)cfg.service_ms[s]/allocation[s]>(double)cfg.service_ms[best]/allocation[best]))best=s;if(best<0)break;allocation[best]++;}
        lock_shared(sem_id); memcpy(shared->staffed,allocation,sizeof allocation); unlock_shared(sem_id);
        int started=0;
        for(int s=0;s<4;s++)for(int n=0;n<allocation[s];n++){
            char a[8];snprintf(a,sizeof a,"%d",s+1);
            launch(s==3?"./operatore_cassa":"./operatore",s==3?NULL:a);started++;
        }
        for(int u=0;u<cfg.users;u++) launch("./utente","0");
        started+=cfg.users;
        for(int n=0;n<started;n++)sem_p(sem_id,SEM_START);
        for(int n=0;n<started;n++)sem_v(sem_id,SEM_GATE);
        printf("Day %d/%d opened (%d workers, %d users).\n",day,cfg.sim_duration,cfg.workers,cfg.users);
        int overload=0;
        for(int minute=0;minute<cfg.day_minutes;minute++){
            long long ns=cfg.minute_ns; struct timespec t={(time_t)(ns/1000000000),(long)(ns%1000000000)}; nanosleep(&t,NULL);
            if(minute>0 && minute%10==0){lock_shared(sem_id);for(int k=0;k<2;k++)for(int d=0;d<cfg.n_dishes[k];d++){shared->portions[k][d]+=cfg.refill[k];if(shared->portions[k][d]>cfg.max_portions[k])shared->portions[k][d]=cfg.max_portions[k];}unlock_shared(sem_id);}
        }
        lock_shared(sem_id); overload=shared->queue[0]+shared->queue[1]+shared->queue[2]+shared->queue[3]>cfg.overload; unlock_shared(sem_id);
        stop_children(); print_stats(day,cfg.sim_duration);
        completed_days++;
        if(overload){termination="overload";break;}
    }
    printf("\nFinal termination cause: %s.\n",termination);
    lock_shared(sem_id);
    long long total_wait=0,total_count=0;
    for(int i=0;i<4;i++){total_wait+=shared->wait_ms[i];total_count+=shared->wait_count[i];}
    printf("Simulation wait averages (ms): all %.2f; first %.2f; second %.2f; coffee %.2f; cashier %.2f\n",
      (double)total_wait/(total_count+1),(double)shared->wait_ms[0]/(shared->wait_count[0]+1),(double)shared->wait_ms[1]/(shared->wait_count[1]+1),(double)shared->wait_ms[2]/(shared->wait_count[2]+1),(double)shared->wait_ms[3]/(shared->wait_count[3]+1));
    printf("Simulation totals: served=%d unserved=%d dishes [first=%d second=%d coffee/dessert=%d] leftovers [first=%d second=%d coffee/dessert=%d] active workers=%d pauses=%d revenue=%.2f; avg revenue/day=%.2f\n",
      shared->served,shared->unserved,shared->distributed[0],shared->distributed[1],shared->distributed[2],shared->leftovers[0],shared->leftovers[1],shared->leftovers[2],shared->active_workers,shared->pauses,shared->revenue,shared->revenue/(completed_days?completed_days:1));
    unlock_shared(sem_id);
    cleanup(); return 0;
}
