#define FTOK_PATH "/root"
#define FTOK_KEY 3450
#define FTOK_MSG_KEY 3451

//maximum running processes
#define SMAX 18

//page size
#define PSIZE	1024
//Page table size
#define PTSIZE	32
//Frame table size
#define FTSIZE 256

//Frame bits
#define NOT_LOADED 0x0
#define LOADED		 0x1
#define DIRTY      0x2

//Page bits
#define REFERENCED 0x1

struct page {
	int frame;
	int bits;			//reference bits
};

struct frame {	//mem_frame
	int page;
	int user;
	int lru_time;
	char bits;
};

enum op { READ=0, WRITE=1, BLOCK=2, CANCEL=3, TERMINATING=4};

struct request_message {
	long mtype;

	int user;	//user index
	int val;	//address value
	int op;		//operation
};

#define REQUEST_MSG_SIZE sizeof(int)*4

struct oss_user {
	int pid, id;

	struct page   page_table[PTSIZE];
	double				page_weights[PTSIZE];

	struct timeval loadt;
};

struct oss {
	sem_t sem;
	struct timeval shared_clock;

	//tell user to exit, if oss is stopped (due to signal or timeout)
	int stop_flag;

  struct frame  frame_table[FTSIZE];

	//process control table
	struct oss_user user_proc[SMAX];
};
