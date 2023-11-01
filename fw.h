#ifndef FW_H
#define FW_H

#include <stddef.h>

#define FW_EVT_ADD    0x002
#define FW_EVT_READ   0x004
#define FW_EVT_WRITE  0x008
#define FW_EVT_WATCH  0x010
#define FW_EVT_DELETE 0x020
#define FW_EVT_CLOSE  0x040
#define FW_EVT_OPEN   0x080
#define FW_EVT_CREATE 0x100
#define FW_EVT_MOVE   0x200

#define FW_EVT_ERR -1
#define FW_EVT_OK  1

typedef struct fwLoop fwLoop;

typedef void fwEvtCallback(fwLoop *fwl, int fd, void *data, int type);

void fwLoopProcessEvents(fwLoop *fwl);
void fwLoopMain(fwLoop *fwl);
void fwLoopStop(fwLoop *fwl);
size_t fwLoopGetProcessedEventCount(fwLoop *fwl);
void fwLoopDeleteEvent(fwLoop *fwl, int fd, int mask);
fwLoop *fwLoopNew(int eventcount, int timeout);
int fwLoopAddEvent(fwLoop *fwl, int fd, int mask, fwEvtCallback *cb, void *data);

#endif // !FW_H
