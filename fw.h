#ifndef FW_H
#define FW_H

#include <stddef.h>

#define FW_EVT_ADD    0x02 //     10
#define FW_EVT_READ   0x04 //    100
#define FW_EVT_WRITE  0x08 //   1000
#define FW_EVT_WATCH  0x10 //  10000
#define FW_EVT_DELETE 0x20 // 100000

#define FW_EVT_ERR -1
#define FW_EVT_OK  1

typedef struct fwLoop fwLoop;

typedef void fwEvtCallback(fwLoop *fwl, int fd, void *data, int type);

void fwLoopProcessEvents(fwLoop *fwl);
void fwLoopMain(fwLoop *fwl);
void fwLoopStop(fwLoop *fwl);
size_t fwLoopGetProcessedEventCount(fwLoop *fwl);
void fwLoopDeleteEvent(fwLoop *fwl, int fd, char *name, int mask);
fwLoop *fwLoopNew(int eventcount, int timeout);
int fwLoopAddEvent(fwLoop *fwl, int fd, char *name, int mask, fwEvtCallback *cb,
                   void *data);

#endif // !FW_H
