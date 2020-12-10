#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/time.h>
#include <semaphore.h>
#include <sys/msg.h>

#include "oss.h"

struct oss *ptr = NULL;
struct oss_user *usr = NULL;

static int xx=0, mx=0;

static struct oss* setup_shm(){
	const key_t key = ftok(FTOK_PATH, FTOK_KEY);
	if(key == -1){
		perror("ftok");
		return NULL;
	}

	const int memid = shmget(key, sizeof(struct oss), 0);
  if(memid == -1){
  	perror("semget");
  	return NULL;
  }

	struct oss * addr = (struct oss *) shmat(memid, NULL, 0);
	if(addr == NULL){
		perror("shmat");
		return NULL;
	}

	return addr;
}

static int setup_msg(){
	const key_t key = ftok(FTOK_PATH, FTOK_MSG_KEY);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	int msgid = msgget(key, 0);
  if(msgid == -1){
  	perror("msgget");
  	return -1;
  }
	return msgid;
}

static int get_xx(const int argc, char * const argv[]){

	if(argc != 3){
		fprintf(stderr, "Usage: user xx mx\n");
		return -1;
	}

	xx = atoi(argv[1]);
	if((xx < 0) | (xx >= SMAX)){
		fprintf(stderr, "Error: xx is invalid\n");
		return -1;
	}

	mx = atoi(argv[2]);
	if((mx < 0) | (mx > 1)){
		fprintf(stderr, "Error: mx is invalid\n");
		return -1;
	}

	return 0;
}

//Setup the memory page weights
static void setup_mem(){
  int i;
  for(i=1; i <= PTSIZE; i++){
    usr->page_weights[i-1] = 1 / i;	//weight is 1 / n
  }
}

//Update the memory page weights after each reference
static void update_mem(){
	int i;
	for(i=1; i <= PTSIZE; i++){
		usr->page_weights[i] += usr->page_weights[i-1];	// add previous weight to current page weight
	}
}

static int create_msg(struct request_message *msg){

	//decide if we read or write
	msg->op = ((rand() % 100) < 70) ? READ : WRITE;	//70 change for read

	int pn = 0;	//selected page number

	if(mx == 0){ //random page number
		pn = rand() % PTSIZE;
	}else{	// weighted page number


		int d = rand() % PTSIZE;
		if(d == 0)	d = 1;	//just to avoid division by zero

		//make a random double number, up to the last weight in table
		const double rand_weight = usr->page_weights[PTSIZE-1] / (double) d;

		//search for the first page with weight higher than w
		for(pn=0; pn < PTSIZE; pn++){
			if(rand_weight <= usr->page_weights[pn]){
				break;
			}
		}
	}
	//generate the address
	msg->val = (pn*PSIZE) + (rand() % PSIZE);

	return 1;
}

static int process_msg(struct request_message *msg){

	switch(msg->op){
		case READ:
		case WRITE:
			update_mem();
			break;

		case CANCEL:
			//do nothing
			return -1;

		case BLOCK:
			//shouldn't happen
			fprintf(stderr, "Error: Processing blocked message\n");
			break;

		default:
			//shouldn't happen
			fprintf(stderr, "Error: Processing unknown message\n");
			break;
	}
	return 0;
}

int main(const int argc, char * const argv[]){
	int term_flag = 0;
	struct request_message msg;

	if(get_xx(argc, argv) == -1){
		return EXIT_FAILURE;
	}

	ptr = setup_shm();
	if(ptr == NULL){
		return EXIT_FAILURE;
	}
	usr = &ptr->user_proc[xx];

	const int msgid = setup_msg();
  if(msgid == -1){
    return EXIT_FAILURE;
  }

	srand(getpid());


	sem_wait(&ptr->sem);

	setup_mem();


	//check if we should terminate
	while((ptr->stop_flag == 0) && (term_flag == 0)){

		sem_post(&ptr->sem);

		//create a read or write mem reference message
		create_msg(&msg);

		//send our mem reference request and wait for reply
		msg.mtype = getppid();	//set OSS, as receiver
		msg.user = xx;	//set our id, as sender
		msgsnd(msgid, (void*)&msg, sizeof(struct request_message), 0);
		msgrcv(msgid, (void*)&msg, sizeof(struct request_message), getpid(), 0);

		if(process_msg(&msg) < 0){
			break;
		}

		sem_wait(&ptr->sem);
	}
	sem_post(&ptr->sem);

	//send final message
	msg.mtype = getppid();	//set OSS, as receiver
	msg.user = xx;	//set our id, as sender
	msg.val = 0;
	msg.op  = TERMINATING;
	msgsnd(msgid, (void*)&msg, REQUEST_MSG_SIZE, 0);

	shmdt(ptr);

	return EXIT_SUCCESS;
}
