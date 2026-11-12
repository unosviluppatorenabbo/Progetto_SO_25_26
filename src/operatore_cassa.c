#include "common.h"
int main(void){
 int sid=semget(ftok(FTOK_PATH,FTOK_SEM),NUM_SEMAFORI,0666),mid=msgget(ftok(FTOK_PATH,FTOK_MSG),0666),hid=shmget(ftok(FTOK_PATH,FTOK_SHM),sizeof(Shared),0666);
 if(sid<0||mid<0||hid<0) return 2;
 Shared *sh=shmat(hid,NULL,0);
 if(sh==(void *)-1) return 2;
 lock_shared(sid);sh->active_workers++;sh->day_active++;unlock_shared(sid);
 sem_v(sid,SEM_START);
 sem_p(sid,SEM_GATE);
 while(sh->active){Message req;if(msgrcv(mid,&req,sizeof req-sizeof(long),CASSA,0)<0){if(errno==EINTR)continue;break;} if(!sh->active)break;
  int avg=sh->cfg.service_ms[3],span=avg/5,duration=avg-span+(span?rand()%(2*span+1):0);long ns=scale_ms(&sh->cfg,duration);struct timespec ts={ns/1000000000,ns%1000000000};nanosleep(&ts,NULL);
  int dishes=req.dish;double cost=(dishes&1?sh->cfg.price[0]:0)+(dishes&2?sh->cfg.price[1]:0)+(req.coffee?sh->cfg.price[2]:0);
  lock_shared(sid);sh->revenue+=cost;sh->day_revenue+=cost;sh->served++;sh->day_served++;if(sh->queue[3]>0)sh->queue[3]--;unlock_shared(sid);
  Reply rep={.type=req.user,.ok=1,.dish=0};msgsnd(mid,&rep,sizeof rep-sizeof(long),0);
 }
 shmdt(sh);return 0;
}
