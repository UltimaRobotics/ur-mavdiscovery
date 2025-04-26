#ifndef __CSSL_H__
#define __CSSL_H__

#include <stdint.h>
#include <signal.h>
#include <termios.h>


typedef void (*cssl_callback_t)(int id, uint8_t *buffer, int len);

typedef struct __cssl_t {
    uint8_t buffer[255];
    int fd;
    struct termios tio;
    struct termios oldtio;
    cssl_callback_t callback;
    int id;
    struct __cssl_t *next;
} cssl_t;

typedef enum {
    CSSL_OK,
    CSSL_ERROR_NOSIGNAL,
    CSSL_ERROR_NOTSTARTED,
    CSSL_ERROR_NULLPOINTER,
    CSSL_ERROR_OOPS,
    CSSL_ERROR_MEMORY,
    CSSL_ERROR_OPEN
} cssl_error_t;

const char *cssl_geterrormsg();
int cssl_geterror();
void cssl_start();
void cssl_stop();
cssl_t *cssl_open(const char *fname, cssl_callback_t callback, int id, int baud, int bits, int parity,int stop);
void cssl_close(cssl_t *serial);
void cssl_setup(cssl_t *serial, int baud, int bits, int parity, int stop);
void cssl_setflowcontrol(cssl_t *serial, int rtscts, int xonxoff);
void cssl_putchar(cssl_t *serial, char c);
void cssl_putstring(cssl_t *serial, char *str);
int cssl_putdata(cssl_t *serial, uint8_t *data, int datalen);
void cssl_drain(cssl_t *serial);
void cssl_settimeout(cssl_t *serial, int timeout);
int cssl_getchar(cssl_t *serial);

int cssl_getdata(cssl_t *serial, uint8_t *buffer, int size);      

#endif 