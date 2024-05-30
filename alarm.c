#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <termios.h>
#include <signal.h>
#include <fcntl.h>

#define TRUE 1
#define FALSE 0

//TODO: open/close/read/write
// 2 threads, one for buttons, second for reading taster?
// interrupt and asynchronous signal -> exit the app TODO: change file 

FILE *pft = NULL;

#define BUFF 20
#define FIVE_MIN_TO_MS 5*60*1000

enum Buttons    {MODE_BUTTON = 0,
		        DECREMENT_BUTTON,
		        INCREMENT_BUTTON,
		        EXIT_BUTTON,
		        BUTTON_NOT_PRESSED};

enum States 	{INIT = 0,
		        STOP,
		        START,
		        INCREMENT,
		        DECREMENT,
		        DO_NOTHING,
		        EXIT};

enum States state = STOP;
enum Buttons buttons = MODE_BUTTON;

unsigned char check_button = 1;
unsigned long long int initial_timer_value = 0;
int en_init = 1;
char mode = 'p';
int gotsignal=0;
struct sigaction action;
int fd;
pthread_t thread_id; 
pthread_t thread2_id;



void sighandler(int signo)
{
    if (signo==SIGIO)
    {
	gotsignal=1;
    }
    return;
}
int print_term(char *msg) {
    if(isatty(fileno(stdin))) { 
        fprintf(stdout, msg);
        return 0;
    }
    fprintf(stderr, "Could not output to terminal, file descriptor %d", fileno(stdin));
    return -1;
}

unsigned long long int decode(char *buff) {
	unsigned long long int timer_value = 0;
	for(int i = 0; i < 8; ++i) {

		timer_value +=(unsigned long long int)buff[i] << (i*8); //56-i*8
		// Debug if needed!
		//iprintf("suma = %llu\n", timer_value);
		//printf("buff[%d] = %d\n", i, buff[i]);
	}
	return timer_value;
}

unsigned long long int read_timer_driver() {
	freopen("/dev/timer", "r+", pft);
	//fseek(pft, 0, SEEK_SET);
	unsigned long long int timer_value = 0;
	size_t len = 8;
    	if(pft == NULL) {
		fprintf(stderr, "failed to open file! [/dev/timer");
        return -1;
    	}
	char *buff = (char*) malloc(len+1);

	getline(&buff, &len, pft);
	//fgets(buff, len, pft);
	if(ferror(pft))
		print_term("Error in reading from file: /dev/timer");	
	timer_value = decode(buff);
	// Debug if needed!
	//fprintf(stdout, "read_timer_driver timer_value = %llu\n", timer_value);
	free(buff);
	//fclose(pft);
	return timer_value/100000;
}

int write_timer_driver(char mode, unsigned int timer_value) {
	freopen("/dev/timer", "w", pft);
	//fseek(pft, 0, SEEK_SET);
	//pft = fopen("/dev/timer", "w+");
	if(pft == NULL) {
		fprintf(stderr, "failed to open file! [/dev/timer");
        return -1;
    }
    fprintf(pft, "%c,%u,%d", mode, timer_value, en_init);
    //fclose(pft);
}

void increment_timer(char mode) { 
    unsigned long long int timer_value = 0;
    timer_value = read_timer_driver();
    //fprintf(stdout, "Before increment: %llu\n", timer_value);
    timer_value += 10*1000;
    //timer_value = timer_value - FIVE_MIN_TO_MS;
    //fprintf(stdout, "Increment timer: %llu\n", timer_value);
    write_timer_driver(mode, timer_value);
    timer_value = read_timer_driver();
    //fprintf(stdout, "After writing to timer: %llu\n", timer_value);
}

void decrement_timer(char mode) { 
    unsigned long long int timer_value = 0;
    timer_value = read_timer_driver();
    //timer_value = timer_value - FIVE_MIN_TO_MS;
    if(timer_value >= 10*1000)
        timer_value -= 10*1000;
    else
	    timer_value = 0;
    write_timer_driver(mode, timer_value);
}

void start_stop_timer(char mode) {
    unsigned int timer_value = 0;
    //TODO: check if reading is needed even, write mode,-1 
    //      so it fails the write in driver
    timer_value = read_timer_driver();
    timer_value = timer_value;
    write_timer_driver(mode, timer_value);
}

void print_time() {
	//fflush(pft);
	unsigned long long int timer_value = read_timer_driver();
	unsigned int ms = 0;
	unsigned int sec = 0;
	unsigned int min = 0;
	unsigned int hh = 0;

	ms = timer_value;
	sec = ms / 1000;
	min = sec / 60;
	hh = min / 60;
	ms %= 1000;
	sec %= 60;;
	min %= 60;
	hh %= 24;

    if(isatty(fileno(stdin))) { 
        fprintf(stdout, "%2d:%2d:%2d:%3d", hh, min, sec, ms);
    }
	else
        fprintf(stderr, "Could not output to terminal, file descriptor %d", fileno(stdin));
}

int read_button(void) {	
	int len = 6;
	char *button = (char*) malloc(len+1);


	FILE *pfb = fopen("/dev/button", "r");	// FILE pointer to the /dev/buttons
	if(pfb == NULL) {
		fprintf(stderr, "failed to open file! [/dev/button]");
		return -1; 
	}

	getline(&button, &len, pfb);
	fclose(pfb);
	button[6] = 0;

	if(!strcmp(button, "0b0000")) {
		check_button = 1;
		buttons = BUTTON_NOT_PRESSED;
	}
	else
	if(!strcmp(button, "0b1000") && check_button) {
		buttons = MODE_BUTTON;
		check_button = 0;
	}
	else
	if(!strcmp(button, "0b0100") && check_button) {
		buttons = DECREMENT_BUTTON;
		check_button = 0;
	}
	else
	if(!strcmp(button, "0b0010") && check_button) {
		buttons = INCREMENT_BUTTON;
		check_button = 0;
	}
	else
	if(!strcmp(button, "0b0001") && check_button) {
		check_button = 0;
		buttons = EXIT_BUTTON;
	}
	//if(!check_button) buttons = BUTTON_NOT_PRESSED;
	
	free(button);
    
    return 0;
}

void change_state(void) {
        if(buttons == MODE_BUTTON) {
            state = START;
	    buttons = BUTTON_NOT_PRESSED;
	}
        else
        if(buttons == DECREMENT_BUTTON) {
            state = DECREMENT;
	    buttons = BUTTON_NOT_PRESSED;
	}
        else
        if(buttons == INCREMENT_BUTTON) {
            state = INCREMENT;
	    buttons = BUTTON_NOT_PRESSED;
	}
        else
        if(buttons == EXIT_BUTTON) {
            state = EXIT;
	    buttons = BUTTON_NOT_PRESSED;
	}
        else
            state = DO_NOTHING;
}

void *thread_check_keyboard(void *vargp) {
	char ch;
	while(TRUE) {
		usleep(1000);
		getchar();
		//system("pause");  
		print_time();
	}
	return NULL;
}

void *thread_check_button(void *vargp) {
	while(TRUE) {
		if (gotsignal)
			state = EXIT;
	    		//return 0;		
		switch (state) {
		    case INIT:
			// initially pause the timer and set value to 0
			system("clear");
			mode = 'p';
			write_timer_driver(mode, 0);
			initial_timer_value = read_timer_driver();	
			print_time();
			break;
		    case START:
			mode = 's';
			initial_timer_value = read_timer_driver();
			en_init = 0;
			start_stop_timer(mode);
			break;
		    case STOP:
			mode = 'p';
			start_stop_timer(mode);
			break;
		    case INCREMENT:
			increment_timer(mode);
			break;
		    case DECREMENT:
			decrement_timer(mode);
			break;
		    case EXIT:
			fclose(pft);
			pthread_cancel(thread2_id);
			pthread_exit(NULL);
			//return 0;
			break;
		    case DO_NOTHING:	
			usleep(1000);
			break;
		    default:
			break;
		}
		
		read_button();
		change_state();
		fflush(stdout);
		//fflush(pft);

	}
	return NULL;
}

int main(void) {
    pft = fopen("/dev/timer", "r+");
    fd = open("/dev/timer",O_RDONLY|O_NONBLOCK);
    if (!fd)
    {
	exit(1);
	printf("Error opening file\n");
    }
    
    action.sa_handler = sighandler;
    sigaction(SIGIO, &action, NULL);
    printf("pid of a current process is: %d\n", getpid());
    fcntl(fd, F_SETOWN, getpid());
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | FASYNC);

    en_init = 1;
    state = INIT;

    pthread_create(&thread2_id, NULL, thread_check_keyboard, NULL);
    pthread_create(&thread_id, NULL, thread_check_button, NULL);
    pthread_join(thread_id, NULL);
    pthread_join(thread2_id, NULL);

    pthread_cancel(thread_id);
    
    //while(TRUE) {
    //    } 
    return 0;
}
