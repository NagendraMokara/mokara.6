#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <sys/shm.h>
#include <time.h>
#include <semaphore.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>

#include "oss.h"

struct oss_stat {
  int reads;
  int writes;
  int evicted;
  int page_faults;
};

static int nop = 0, nmax = 100; 
static int s = 0, smax = SMAX;
static int e = 0, emax = 0;   
static int tmax = 2;   
static int v = 0; //verbose

static struct oss_stat ostat; //oss statistics

static int memid = -1;  //shared memory
static int msgid = -1;  //message queue

static struct oss * ptr = NULL;

//this is our queue of blocked message requests
static struct request_message bmsg[SMAX];  //blocked messages
static unsigned int bmsg_len = 0;

static int loop_flag = 1;
static int user_mx = 0; //memory address scheme

//frame bitmap
static unsigned char bitmap[(FTSIZE / 8) + 1];

static void wait_children(){
  pid_t pid;
  int status;

  while((pid = waitpid(-1, &status, WNOHANG)) > 0){

    //clear from pids[]
    int i;
    for(i=0; i < SMAX; i++){
      if(ptr->user_proc[i].pid == pid){
        ptr->user_proc[i].pid = 0;

        //clear the page table
        int j;
        for(j=0; j < PTSIZE; j++){
          const int f = ptr->user_proc[i].page_table[j].frame;
          if(f > 0){
            //clear the frame
            struct frame * frame = &ptr->frame_table[f];
            frame->bits = NOT_LOADED;
            frame->page = frame->user = -1;
            frame->lru_time = 0;

            bitmap[f / 8] &= ~(1 << (f % 8));
          }

          //clear the page
          ptr->user_proc[i].page_table[j].frame   = -1;
          ptr->user_proc[i].page_table[j].bits =  0;
        }
      }
    }

    --s;
    if(++e >= emax){  //if all exited
      loop_flag = 0;  //stop the loop
    }

    //just in case, if a process is waiting on the timer
    /*sem_wait(&ptr->sem);
    ptr->shared_clock.tv_sec++;
    sem_post(&ptr->sem);*/
  }
}

static int term_children(){
  int i;
  struct request_message msg;


  sem_wait(&ptr->sem);
  ptr->stop_flag = 1;
  sem_post(&ptr->sem);

  for(i=0; i < SMAX; ++i){
    if(ptr->user_proc[i].pid == 0)
      continue;

    /*if(ptr->user_proc[i].pid > 0){
      kill(ptr->user_proc[i].pid, SIGTERM);
    }*/

    msg.mtype = ptr->user_proc[i].pid;
    msg.op = CANCEL;
    msgsnd(msgid, (void*)&msg, REQUEST_MSG_SIZE, 0);
  }

  while(s > 0){
    wait_children();
  }

  return 0;
}

static void sig_handler(const int sig){

  switch(sig){
    case SIGINT:
    case SIGALRM:
      loop_flag = 0;

      sem_wait(&ptr->sem);
      ptr->stop_flag = 1;
      sem_post(&ptr->sem);
      break;

    case SIGCHLD:
      wait_children();
      break;

    default:
      break;
  }
}

static void clean_shm(){

  sem_destroy(&ptr->sem);
  msgctl(msgid, IPC_RMID, NULL);
  shmctl(memid, IPC_RMID, NULL);
  shmdt(ptr);
}

static int find_free_oss_user(){
  int i;
  for(i=0; i < SMAX; i++){
    if(ptr->user_proc[i].pid == 0){
      return i;
    }
  }
  return -1;
}

static int exec_user(){
  char xx[20], mx[20];

  const int user_index = find_free_oss_user();
  if(user_index < 0){
    //this should not happen, since we exec only when s < smax
    fprintf(stderr, "OSS: user_index == -1\n");
    return -1;
  }

  const pid_t pid = fork();
  switch(pid){
    case -1:
      perror("fork");
      break;

    case 0:
      snprintf(xx, 20, "%i", user_index);
      snprintf(mx, 20, "%i", user_mx);

      setpgid(getpid(), getppid());
      execl("user_proc", "user_proc", xx, mx, NULL);

      perror("execl");
      exit(EXIT_FAILURE);
      break;

    default:
      //save process pid
      printf("Master: Started P%d at time %ld:%ld\n",
        nop, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      ptr->user_proc[user_index].pid = pid;
      ptr->user_proc[user_index].id  = nop;
      break;
  }

	return pid;
}

static int setup_args(const int argc, char * const argv[]){

  int option;
	while((option = getopt(argc, argv, "nop:s:t:h:m:v")) != -1){
		switch(option){
			case 'h':
        fprintf(stderr, "Usage: master [-n x] [-s x] [-t x] infile.txt\n");
        fprintf(stderr, "-nop 100 Processes to start\n");
        fprintf(stderr, "-s 18 Processes to rununing\n");
        fprintf(stderr, "-t 2 Runtime\n");
        fprintf(stderr, "-m 0|1 Memory address request scheme\n");
        fprintf(stderr, "-v   Verbose\n");
        fprintf(stderr, "-h Show this message\n");
				return -1;

      case 'nop':  nmax	= atoi(optarg); break;
			case 's':  smax	= atoi(optarg); break;
      case 't':  tmax	= atoi(optarg); break;
      case 'm':  user_mx = atoi(optarg); break;
      case 'v':  v = 1;               break;

      default:
				fprintf(stderr, "Master: Error: Invalid option %c\n", option);
				return -1;
		}
	}

  if( (user_mx < 0) || (user_mx > 1)   ){
    fprintf(stderr, "Error: -m invalid\n");
    return -1;
  }

  if( (smax <= 0) || (smax > 18)   ){
    fprintf(stderr, "Error: -s invalid\n");
    return -1;
  }
  emax = nmax;

  return 0;
}

static struct oss * setup_shm(){

	key_t key = ftok(FTOK_PATH, FTOK_KEY);
	if(key == -1){
		perror("ftok");
		return NULL;
	}

	memid = shmget(key, sizeof(struct oss), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
  if(memid == -1){
  	perror("shmget");
  	return NULL;
  }

	struct oss * addr = (struct oss *) shmat(memid, NULL, 0);
	if(addr == NULL){
		perror("shmat");
		return NULL;
	}

  bzero(addr, sizeof(struct oss));

  if(sem_init(&addr->sem, 1, 1) == -1){
    perror("sem_init");
    return NULL;
  }

	return addr;
}

static int setup_msg(){

	key_t key = ftok(FTOK_PATH, FTOK_MSG_KEY);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	msgid = msgget(key, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
  if(msgid == -1){
  	perror("shmget");
  	return -1;
  }
  return msgid;
}

static int shared_clock_update(){
  struct timeval timestep, temp;

  timestep.tv_sec = 0;
  timestep.tv_usec = 1000;

  sem_wait(&ptr->sem);
  timeradd(&ptr->shared_clock, &timestep, &temp);
  ptr->shared_clock = temp;
  sem_post(&ptr->sem);

  return 0;
}

//Setup the system resource descriptors
static void setup_mem(){
  int i,j;

  bzero(bitmap, sizeof(bitmap));

  //initialize the page and frame tables
	for(i=0; i < SMAX; i++){
    for(j=0; j < PTSIZE; j++){
      ptr->user_proc[i].page_table[j].frame   = -1;
      ptr->user_proc[i].page_table[j].bits = 0;
    }
    ptr->user_proc[i].loadt.tv_sec = 0;
    ptr->user_proc[i].loadt.tv_usec = 0;
  }

	//clear frame table
	for(i=0; i < FTSIZE; i++){
    //clear the frame
    ptr->frame_table[i].bits = NOT_LOADED;
    ptr->frame_table[i].page   = ptr->frame_table[i].user = -1;
    ptr->frame_table[i].lru_time = 0;
    bitmap[i / 8] &= ~(1 << (i % 8));
  }
}

static void list_memory(){
  int i;

  printf("Current System Memory at time %ld:%ld\n",
    ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

  printf("\t\tOccupied\tDidrtyBit\ttimeStamp\n");
  for(i=0; i < FTSIZE; i++){
    int dirty_bit = (ptr->frame_table[i].bits & DIRTY) ? 1 : 0;
    printf("Frame %2d\t\tYes\t\t%7d\t%8d\n", i, dirty_bit, ptr->frame_table[i].lru_time);
  }
  printf("\n");
}

static int request_block(struct request_message *m){
  if(bmsg_len < SMAX){  //if we have space in blocked queue
    memcpy(&bmsg[bmsg_len++], m, sizeof(struct request_message));
    return 0;
  }else{
    //this shouldn't happen, but return error
    return -1;
  }
}

static int unblock_requests(){
  int i, loaded = 0;

  //for each request in blocked queue
  for(i=0; i < bmsg_len; i++){

    struct request_message * m = &bmsg[i];
    struct oss_user * usr = &ptr->user_proc[m->user];

    //if memory page is loaded
    if( timercmp(&ptr->shared_clock, &usr->loadt, >) ){ //if page load time has passed

      printf("Master P%d address %d loaded at system time %lu:%li\n",
        usr->id, m->val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      //unblock the waiting user
      m->mtype = usr->pid;
      //m->op = REQUEST;
      if(m->op == READ){
        ostat.reads++;
      }else{
        ostat.writes++;
      }
      msgsnd(msgid, (void*)m, REQUEST_MSG_SIZE, 0);

      usr->loadt.tv_sec = 0; usr->loadt.tv_usec = 0;
      ++loaded;

      //remove message from queue
      if(i != (bmsg_len-1)){  //if we are not at last message
        //replace it with last message
        memcpy(&bmsg[i], &bmsg[bmsg_len-1], sizeof(struct request_message));
        --i;
      }
      --bmsg_len;
    }
  }

  return loaded;  //return number of pages that have been loaded
}

//Find a page to evict and free a frame
static int evict(){
  int i, f = 0;
  for(i=0; i < FTSIZE; i++){
    if(ptr->frame_table[i].lru_time > ptr->frame_table[f].lru_time){
      f = i;
    }
  }
  return f;
}

static int page_fault(struct oss_user * usr, struct request_message * m){

	ostat.page_faults++;

  const int pidx = m->val / PSIZE;

	//find a frame that is not used
  int i, f = -1;
  for(i=0; i < FTSIZE; i++){
    const int bit = i % 8;
  	if( ((bitmap[i / 8] & (1 << bit)) >> bit) == 0){
      f = i;
      break;
    }
  }

  if(f >= 0){
		printf("Master using free frame %d for P%d page %d at system time %lu:%li\n",
      f, usr->id, pidx, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

  }else{	//no frame is free

		ostat.evicted++;
	  f = evict(ptr->frame_table);

	  struct frame * fr = &ptr->frame_table[f];
	  struct page * vp = &ptr->user_proc[fr->user].page_table[fr->page];

	  printf("Master evicting page %d of P%d at system time %li:%lu\n",
	      fr->page, fr->user, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

	  vp->bits = 0;

	  printf("Master clearing frame %d and swapping in P%d page %d at system time %li:%lu\n",
	    vp->frame, usr->id, pidx, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

		if(fr->bits & DIRTY){
      struct timeval tv, tv2 = ptr->shared_clock;
      tv.tv_sec = 0; tv.tv_usec = 14;
      timeradd(&tv, &tv2, &ptr->shared_clock);

			printf("Master adding additional dirty bit time to clock for frame %d at system time %li:%lu\n",
				vp->frame, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      //remove dirty bit
      fr->bits &= ~DIRTY;
		}

	  f = vp->frame;
    //clear the frame
    ptr->frame_table[f].bits = NOT_LOADED;
    ptr->frame_table[f].page = ptr->frame_table[f].user = -1;
    ptr->frame_table[f].lru_time = 0;
    bitmap[f / 8] &= ~(1 << (f % 8));

	  vp->frame = -1;
	}

  return f;
}

static int page_load(struct oss_user * usr, struct request_message *m){
  int rv;
  struct timeval tv, tv2;

  const int pidx = m->val / PSIZE;
  if((pidx < 0) || (pidx > PTSIZE)){
    return -1;
  }

	struct page * vp = &usr->page_table[pidx];

  if(vp->frame < 0){  //if page is not in a frame

    printf("OSS pagefault at address %d at system time %li:%lu\n",
      m->val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

		//update page
  	vp->frame = page_fault(usr, m);
  	vp->bits |= REFERENCED;

		//update frame
    bitmap[vp->frame / 8] |= (1 << (vp->frame % 8));  //set the bit
    ptr->frame_table[vp->frame].bits = LOADED;
  	ptr->frame_table[vp->frame].page = pidx;
  	ptr->frame_table[vp->frame].user = m->user;

    //loading takes time
    usr->loadt.tv_sec = 0; usr->loadt.tv_usec = 0;
    tv.tv_sec = 0; tv.tv_usec = 14;
    timeradd(&tv, &ptr->shared_clock, &usr->loadt);

    request_block(m);

  	rv = 0;

  }else{
    tv.tv_sec = 0; tv.tv_usec = 10;
    tv2 = ptr->shared_clock;
    timeradd(&tv, &tv2, &ptr->shared_clock);

    //update the eviction policy statistics
    int i;
    for(i=0; i < FTSIZE; i++){
      ptr->frame_table[i].lru_time++;
    }
    ptr->frame_table[vp->frame].lru_time--;

    rv = 1;
  }

  return rv;
}

static int process_request(){
  int respond = 0, rv=0;
  struct request_message m;

  if(msgrcv(msgid, (void*)&m, sizeof(struct request_message), getpid(), IPC_NOWAIT) < 0){
    if(errno == ENOMSG){
      return 0;
    }else{
      perror("msgrcv");
      return -1;
    }
  }

  struct oss_user * usr = &ptr->user_proc[m.user];

  if(m.op == READ){
    ostat.reads++;
    printf("Master has detected Process P%d reading address 0x%d at time %ld:%ld\n",
      usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

    rv = page_load(usr, &m);
    switch(rv){
      case -1:  //invalid address
        respond = 1;
        printf("Master has denied Process P%d invalid address 0x%d at time %ld:%ld\n",
              usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);
        break;

      case 0:   //blocked for loading from dist
        respond = 0;
        printf("Master has blocked Process P%d reading address 0x%d at time %ld:%ld\n",
              usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);
        break;

      case 1:   //page is ready
      default:
        respond = 1;
        printf("Master has given Process P%d to read address 0x%d at time %ld:%ld\n",
            usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);
        break;
    }

  }else if(m.op == WRITE){
    ostat.writes++;

    printf("Master has detected Process P%d writing address 0x%d at time %ld:%ld\n",
      usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);


    rv = page_load(usr, &m);
    switch(rv){
      case -1:  //invalid address
        respond = 1;
        printf("Master has denied Process P%d invalid address 0x%d at time %ld:%ld\n",
              usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);
        break;

      case 0:   //blocked for loading from dist
        respond = 0;
        printf("Master has blocked Process P%d writing address 0x%d at time %ld:%ld\n",
              usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);
        break;

      case 1:   //page is ready
      default:
        respond = 1;
        printf("Master has given Process P%d to write address 0x%d at time %ld:%ld\n",
            usr->id, m.val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

        //frame gets dirty after write
        int pn = m.val / PSIZE;
        int f = usr->page_table[pn].frame;
        ptr->frame_table[f].bits |= DIRTY;
        break;
    }

  }else if(m.op == TERMINATING){

    printf("Master has acknowledged Process P%d is terminating at time %ld:%ld\n",
      usr->id, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

    printf("\tReleasing frames: ");
    int j;
    for(j=0; j < PTSIZE; j++){
      const int f = usr->page_table[j].frame;
      if(f > 0){

        printf("%d ", f);

        //clear the frame
        struct frame * frame = &ptr->frame_table[f];
        frame->bits = NOT_LOADED;
        frame->page = frame->user = -1;
        frame->lru_time = 0;

        bitmap[f / 8] &= ~(1 << (f % 8));
      }

      //clear the page
      usr->page_table[j].frame   = -1;
      usr->page_table[j].bits =  0;
    }
    printf("\n");

    wait_children();
    respond = 0;

  }else{

    printf("Master has detected invalid message from Process P%d at time %ld:%ld\n",
      usr->id, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

    m.op = CANCEL;
    respond = 1;  //send reply to user proc
  }

  if(respond){
    m.mtype = usr->pid;
    msgsnd(msgid, (void*)&m, REQUEST_MSG_SIZE, 0);
  }

  return 0;
}

static void list_mstat(){

  const unsigned int refs = ostat.reads + ostat.writes;
  printf("References # %d\n", refs);
	printf("Reads # %d\n", ostat.reads);
	printf("Writes # %d\n", ostat.writes);

	printf("Evictions # %d\n", ostat.evicted);
	printf("Page Faults # %d\n", ostat.page_faults);

	printf("Memory access / second # %.2f\n", (float) refs / ptr->shared_clock.tv_sec);
	printf("Page Faults / Reference # %.2f\n", (float)ostat.page_faults / refs);
}

int main(const int argc, char * const argv[]){

  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, sig_handler);
  signal(SIGCHLD, sig_handler);
  signal(SIGALRM, sig_handler);

  ptr = setup_shm();
  if(ptr == NULL){
    clean_shm();
    return EXIT_FAILURE;
  }

  if(setup_msg() == -1){
    clean_shm();
    return EXIT_FAILURE;
  }

  if(setup_args(argc, argv) == -1){
    clean_shm();
    return EXIT_FAILURE;
  }

  setup_mem();
  ptr->stop_flag = 0;

  //clear the statistics
  bzero(&ostat, sizeof(struct oss_stat));

  alarm(tmax);  /* setup alarm after arguments are processes*/

  int last_refs = 0;
	while(loop_flag && (e < emax)){

    //if we can, we start a new process
    if( (nop < nmax) && (s < smax)  ){
      const pid_t user_pid = exec_user();
      if(user_pid > 0){
        ++nop; ++s;  /* increase count of processes started */
      }
    }

    process_request();
    unblock_requests();

    shared_clock_update();

    //show table of current resource allocations
    if(v){
      const unsigned int refs = ostat.reads + ostat.writes;
      if((last_refs + 1000) < refs){ //on each 1000 messages
        list_memory();
        last_refs = refs;
      }
    }
	}

  //show the results
  list_mstat();

  term_children();
	clean_shm();
  return EXIT_SUCCESS;
}
