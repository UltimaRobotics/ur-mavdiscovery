
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "cssl.h"

static int CSSL_SIGNAL=0;
static int cssl_started=0;

static struct sigaction sa;
static struct sigaction oldsa;

static cssl_t *head=0;

static const char *cssl_errors[]= {
    "cssl: OK",
    "cssl: there's no free signal",
    "cssl: not started",
    "cssl: null pointer",
    "cssl: oops",
    "cssl: out of memory",
    "cssl: cannot open file"
};

static cssl_error_t cssl_error=CSSL_OK;

static void cssl_handler(int signo, siginfo_t *info, void *ignored);

const char *cssl_geterrormsg()
{
    return cssl_errors[cssl_error];
}

int cssl_geterror()
{
    return cssl_error;
}

void cssl_start()
{
    int sig;
    if (cssl_started) { return; }

    /* Here we scan for unused real time signal */
    sig=SIGRTMIN;

    do {
	
	/* get old sigaction */
	sigaction(sig,0,&oldsa);
	
	/* if signal's handler is empty */
	if (oldsa.sa_handler == 0) 
	{
	    /* set the signal handler, and others */
	    CSSL_SIGNAL=sig;
	    sa.sa_sigaction = cssl_handler;
	    sa.sa_flags = SA_SIGINFO;
	    sa.sa_restorer = NULL;
	    sigemptyset(&sa.sa_mask);
	    sigaction(CSSL_SIGNAL,&sa,0);

	    /* OK, the cssl is started */
	    cssl_started=1;
	    cssl_error=CSSL_OK;
	    return;
	} else {
	    /* signal handler was not empty, 
	       restore original */
	    sigaction(CSSL_SIGNAL,&oldsa,0);
	}
	sig++;
    } while(sig<=SIGRTMAX);
    

    /* Sorry, there's no free signal */
    cssl_error=CSSL_ERROR_NOSIGNAL;
    
}

/* stops the cssl */
void cssl_stop()
{
    /* if not started we do nothing */
    if (!cssl_started)
	return;

    /* we close all ports, and free the list */
    while (head)
	cssl_close(head);

    /* then we remove the signal handler */
    sigaction(CSSL_SIGNAL,&oldsa,NULL);

    /* And at least : */
    cssl_started=0;
    cssl_error=CSSL_OK;
}

/*-------------------------------------
 * Basic port operation - open/close
 */


/* opens the port */
cssl_t *cssl_open(const char *fname,
		  cssl_callback_t callback,
		  int id,
		  int baud,
		  int bits,
		  int parity,
		  int stop)
{
    cssl_t *serial;

    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return NULL;
    }
    
    /* create new cssl_t structure */
    serial=calloc(1,sizeof(cssl_t));

    /* oops, no memory */
    if (!serial) {
	cssl_error=CSSL_ERROR_MEMORY;
	return 0;
    }

    /* opening the file */
    if(callback) {
	/* user wants event driven reading */
	serial->fd=open(fname,O_RDWR|O_NOCTTY|O_NONBLOCK);
	fcntl(serial->fd,F_SETSIG,CSSL_SIGNAL);
	fcntl(serial->fd,F_SETOWN,getpid());
	fcntl(serial->fd,F_SETFL,O_ASYNC|O_NONBLOCK );
    } else {
	/* the read/write operations will be bloking */
	serial->fd=open(fname,O_RDWR|O_NOCTTY|O_NDELAY);
    }

    /* oops, cannot open */
    if (serial->fd == -1) {
	cssl_error=CSSL_ERROR_OPEN;
	free(serial);
	return NULL;
    }

    /* we remember old termios */
    tcgetattr(serial->fd,&(serial->oldtio));
    
    /* now we set new values */
    cssl_setup(serial,baud,parity,bits,stop);

    /* and id */
    serial->id=id;

    /* then set the callback */
    serial->callback=callback;
    
    /* we add the serial to our list */
    serial->next=head;
    head=serial;
	
    cssl_error=CSSL_OK;

    return serial;
}


/* closes file, removes serial from the list and frees it */
void cssl_close(cssl_t *serial)
{
    cssl_t *cur;
    
    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return;
    }
    
    /* first we flush the port */
    tcflush(serial->fd,TCOFLUSH); 
    tcflush(serial->fd,TCIFLUSH); 
    
    /* then we restore old settings */
    tcsetattr(serial->fd,TCSANOW,&(serial->oldtio));
    
    /* and close the file */
    close(serial->fd);
    
    /* now we can remove the serial from the list */

    if (head==serial) {
	head=serial->next;
	free(serial);
	cssl_error=CSSL_OK;
	return;
    }

    for (cur=head;cur;cur=cur->next) {
	if (cur->next==serial) {
	    cur->next=serial->next;
	    free(serial);
	    cssl_error=CSSL_OK;
	    return;
	}
    }

    /* we should never reach there,
       it means, that serial was not found in the list */
    cssl_error=CSSL_ERROR_OOPS;
}


/*-------------------------------------
 * Port setup
 */

/* sets up the port parameters */
void cssl_setup(cssl_t *serial,
		   int baud,
		   int bits,
		   int parity,
		   int stop)
{
    tcflag_t baudrate;
    tcflag_t databits;
    (void)databits;
    tcflag_t stopbits;
    (void)stopbits;
    tcflag_t checkparity;
    (void)checkparity;

    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return;
    }    

    /* get the propr baudrate */
    switch (baud) {
    case 75:
	baudrate=B75;
	break;
    case 110:
	baudrate=B110;
	break;
    case 150:
	baudrate=B150;
	break;
    case 300:
	baudrate=B300;
	break;
    case 600:
	baudrate=B600;
	break;
    case 1200:
	baudrate=B1200;
	break;
    case 2400:
	baudrate=B2400;
	break;
    case 4800:
	baudrate=B4800;
	break;
    case 9600:
	baudrate=B9600;
	break;
    case 19200:
	baudrate=B19200;
	break;
    case 38400:
	baudrate=B38400;
	break;
    case 57600:
	baudrate=B57600;
	break;
    case 115200:
	baudrate=B115200;
	break;
    default:
	baudrate=B9600;
    }

    /* databits */
    switch (bits) {
    case 7:
	databits=CS7;
	break;
    case 8:
	databits=CS8;
	break;
    default:
	databits=CS8;
    }
    
    /* parity, */
    switch (parity) {
    case 0:
	checkparity=0;
	break;
    case 1:   //odd
	checkparity=PARENB|PARODD;
	break;
    case 2:
	checkparity=PARENB;
	break;
    default:
	checkparity=0;
    }
    
    /* and stop bits */
    switch (stop) {
    case 1:
	stopbits=0;
	break;
    case 2:
	stopbits=CSTOPB;
	break;
    default:
	stopbits=0;
    }
    
    /* now we setup the values in port's termios */
    tcgetattr(serial->fd, &serial->tio);
    serial->tio.c_cflag |= CLOCAL | CREAD;
    serial->tio.c_cflag &= ~CSIZE;
    serial->tio.c_cflag |= CS8;
    serial->tio.c_cflag &= ~PARENB;
    serial->tio.c_cflag &= ~CSTOPB;
    serial->tio.c_cflag &= ~CRTSCTS;
    serial->tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    serial->tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    serial->tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    serial->tio.c_oflag &= ~OPOST;
    serial->tio.c_cc[VMIN] = 0;
    serial->tio.c_cc[VTIME] = 5;
    cfsetispeed(&serial->tio, baudrate);
    cfsetospeed(&serial->tio, baudrate);
    tcsetattr(serial->fd, TCSANOW, &serial->tio);

    /* we flush the port */
    tcflush(serial->fd,TCOFLUSH);
    tcflush(serial->fd,TCIFLUSH);
    
    /* we send new config to the port */
    tcsetattr(serial->fd,TCSANOW,&(serial->tio));

    cssl_error=CSSL_OK;
}

void cssl_setflowcontrol(cssl_t *serial,
			 int rtscts,
			 int xonxoff)
{
    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return;
    }    

    /* We setup rts/cts (hardware) flow control */
    if (rtscts) {
	serial->tio.c_cflag |= CRTSCTS;
    } else {
	serial->tio.c_cflag &= ~CRTSCTS;
    }
    
    /* We setup xon/xoff (soft) flow control */
    if (xonxoff) {
	serial->tio.c_iflag |= (IXON|IXOFF);
    } else {
	serial->tio.c_iflag &= ~(IXON|IXOFF);
    }
    
    tcsetattr(serial->fd,TCSANOW,&(serial->tio));

    cssl_error=CSSL_OK;
}



/* Blocking mode: sets the timeout in 
   hundreds of miliseconds */
void cssl_settimeout(cssl_t *serial, int timeout)
{
    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return;
    }    

    serial->tio.c_cc[VTIME]=timeout;
    
    tcsetattr(serial->fd,TCSANOW,&(serial->tio));

    cssl_error=CSSL_OK;
}

void cssl_putchar(cssl_t *serial,
		     char c)
{
    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return;
    }    

    write(serial->fd,&c,1);
}

void cssl_putstring(cssl_t *serial,
		     char *str)
{
    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return;
    }    
    write(serial->fd,str,strlen(str));
}

int cssl_putdata(cssl_t *serial,
		  uint8_t *data,
		  int datalen)
{
    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return -1 ;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return -1 ;
    }    

    write(serial->fd,data,datalen);
    return 0;
}

void cssl_drain(cssl_t *serial)
{
    if (!cssl_started) {
	cssl_error=CSSL_ERROR_NOTSTARTED;
	return;
    }
    
    if (!serial) {
	cssl_error=CSSL_ERROR_NULLPOINTER;
	return;
    }    

    tcdrain(serial->fd);
}

int cssl_getchar(cssl_t *serial)
{
    int result;
    uint8_t c;
    
    result=read(serial->fd,&c,sizeof(c));
    if (result<=0)
	return -1;
    
    return c;
}

int cssl_getdata(cssl_t *serial,
		 uint8_t *buffer,
		 int size)
{
    return read(serial->fd,buffer,size);
}

void cssl_handler(int signo, siginfo_t *info, void *ignored)
{
    cssl_t *cur;
    int n;int ext_ascii_bytes; uint8_t rx_buf[280];
    if (info->si_code==POLL_IN) {
	for(cur=head;cur;cur=cur->next) {
	    if (cur->fd==info->si_fd) {
        ext_ascii_bytes =  read(cur->fd, rx_buf, sizeof(rx_buf));
		n=read(cur->fd,cur->buffer,255);
        if ((ext_ascii_bytes > 0)&&((cur->callback))){
            cur->callback(cur->id,rx_buf,ext_ascii_bytes);
            return;
        }
        else if ((n>0)&&(cur->callback))
		    cur->callback(cur->id,cur->buffer,n);
		return;
	    }
	}
    }
}

