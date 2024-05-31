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
#define START_F 1
#define STOP_F 2
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

FILE *pft = NULL;

unsigned char mode_bit = START_F; // used to change START/STOP
unsigned char check_button = 1; //
//unsigned long long int last_value = 0; //
int en_init = 1;
char mode = 'p';
// asynchronous signal
int gotsignal=0;
struct sigaction action;
int fd;
// threads:
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
	}
	return timer_value;
}

unsigned long long int read_timer_driver() {
	freopen("/dev/timer", "r+", pft);
	unsigned long long int timer_value = 0;
	size_t len = 8;
    	if(pft == NULL) {
		fprintf(stderr, "failed to open file! [/dev/timer");
        return -1;
    	}
	char *buff = (char*) malloc(len+1);

	getline(&buff, &len, pft);
	if(ferror(pft))
		print_term("Error in reading from file: /dev/timer");	
	timer_value = decode(buff);
	free(buff);
	return timer_value/100000;
}

int write_timer_driver(char mode, unsigned int timer_value) {
	freopen("/dev/timer", "w", pft);
	if(pft == NULL) {
		fprintf(stderr, "failed to open file! [/dev/timer");
        return -1;
    }
    fprintf(pft, "%c,%u,%d", mode, timer_value, en_init);
}

void increment_timer(char mode) { 
    unsigned long long int timer_value = 0;
    timer_value = read_timer_driver();
    timer_value += 10*1000;
    write_timer_driver(mode, timer_value);
}

void decrement_timer(char mode) { 
    unsigned long long int timer_value = 0;
    timer_value = read_timer_driver(); 
    if(timer_value >= 10*1000)
	timer_value -= 10*1000;
    else
	timer_value = 0;
    write_timer_driver(mode, timer_value);
}

void start_stop_timer(char mode) {
    unsigned int timer_value = 0;
    timer_value = read_timer_driver();
    timer_value = timer_value;
    write_timer_driver(mode, timer_value);
}

void print_time() {
	unsigned long long int timer_value = read_timer_driver();
	unsigned int ms = 0;
	unsigned int sec = 0;
	unsigned int min = 0;
	unsigned int hh = 0;
	
	//convert counter value to time format
	ms = timer_value;
	sec = ms / 1000;
	min = sec / 60;
	hh = min / 60;
	ms %= 1000;
	sec %= 60;;
	min %= 60;
	hh %= 24;

    if(isatty(fileno(stdin))) { 
	system("clear");
        fprintf(stdout, "%2d:%2d:%2d:%2d", hh, min, sec, ms);
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
	
	free(button);
    
    return 0;
}

void change_state(void) {
        if(buttons == MODE_BUTTON && mode_bit == START_F) {
            mode_bit = STOP_F;
            state = START;
	    buttons = BUTTON_NOT_PRESSED;
	}
        else
        if(buttons == MODE_BUTTON && mode_bit == STOP_F) {
            mode_bit = START_F;
            state = STOP;
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
		print_time();
	}
	return NULL;
}

void *thread_check_button(void *vargp) {
	while(TRUE) {
		if (gotsignal)
			state = EXIT;
		switch (state) {
		    case INIT:
			mode = 'p';
			write_timer_driver(mode, 0); // time 0 at start
			print_time();
			break;
		    case START:
			mode = 's';
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

	}
	return NULL;
}

int main(void) {
    pft = fopen("/dev/timer", "r+"); // initialize FILE pointer for timer device
    fd = open("/dev/timer",O_RDONLY|O_NONBLOCK); // used for asynchronous signaling
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

    pthread_create(&thread2_id, NULL, thread_check_keyboard, NULL); // thread for printing time
    pthread_create(&thread_id, NULL, thread_check_button, NULL); // thread for managing app
    pthread_join(thread_id, NULL);
    pthread_join(thread2_id, NULL);

    pthread_cancel(thread_id);
    
    return 0;
}
