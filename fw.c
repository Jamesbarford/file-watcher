#include <sys/event.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fw.h"

/* Events to fire for an event, this is non exhaustive */
typedef struct fwEvt {
    int fd;
    int mask;
    fwEvtCallback *read;
    fwEvtCallback *write;
    fwEvtCallback *watch;
    void *data;
} fwEvt;

/* the loop itself */
typedef struct fwLoop {
    int max;
    int count;
    fwEvt *idle;
    fwEvt *active;
    void *state; /* Allow for generic implementation of state */
    int timeout;
    int fd;
    size_t processed;
    int run;
} fwLoop;

#define fwLoopGetState(fwl) ((fwl)->state)

#define fwEvtWatch(ev, fwl, fd, mask) \
    ((ev)->watch((fwl), (fd), (ev)->data, (mask)))

/** ===========================================================================
 * MAC OS implementation - kqueue
 * ===========================================================================*/
#if defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6) || \
        defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

typedef struct fwEvtState {
    int kfd;
    struct kevent *events;
} fwEvtState;

#define __kevent(kfd, ev) (kevent((kfd), (ev), 1, NULL, 0, NULL))

static int fwStateCreate(fwLoop *fwl) {
    fwEvtState *es;

    if ((es = malloc(sizeof(fwEvtState))) == NULL) {
        goto error;
    }

    if ((es->events = malloc(sizeof(struct kevent) * fwl->count)) == NULL) {
        goto error;
    }

    if ((es->kfd = kqueue()) == -1) {
        goto error;
    }

    fwl->state = es;

    return FW_EVT_OK;

error:
    if (es && es->events) {
        free(es->events);
    }
    if (es) {
        free(es);
    }
    return FW_EVT_ERR;
}

static void fwStateRelease(fwLoop *fwl) {
    if (fwl) {
        fwEvtState *es = fwLoopGetState(fwl);
        if (es) {
            close(es->kfd);
            free(es->events);
            free(es);
        }
    }
}

/* Add a something to watch to kqueue */
static int fwStateAdd(fwLoop *fwl, int fd, int mask) {
    fwEvtState *es = fwLoopGetState(fwl);
    struct kevent change;

    /* WATCH is the most generic one, adding everything for now */
    if (mask & FW_EVT_WATCH) {
        EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB, 0, 0);
        if (kevent(es->kfd, &change, 1, NULL, 0, NULL) == -1) {
            return FW_EVT_ERR;
        }
    }
    return FW_EVT_OK;
}

static void fwStateDelete(fwLoop *fwl, int fd, int mask) {
    fwEvtState *es = fwl->state;
    struct kevent event;

    /* Remove this event for the kqueue */
    EV_SET(&event, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    __kevent(es->kfd, &event);
}

/* Check for activity on a file descriptor */
static int fwPoll(fwLoop *fwl) {
    fwEvtState *es = fwLoopGetState(fwl);
    int fdcount = 0;

    fdcount = kevent(es->kfd, NULL, 0, es->events, fwl->count, NULL);

    if (fdcount == -1) {
        return FW_EVT_ERR;
    }

    if (fdcount > 0) {
        for (int i = 0; i < fdcount; ++i) {
            int newmask = 0;
            struct kevent *change = &es->events[i];

            /* These are treated as watch events */
            if (change->fflags & (NOTE_WRITE | NOTE_EXTEND)) {
                newmask |= FW_EVT_WATCH;
            }

            /* The caller can determine what to do with a delete */
            if (change->fflags & (NOTE_DELETE)) {
                newmask |= FW_EVT_DELETE;
            }

            fwl->active[i].fd = change->ident;
            fwl->active[i].mask = newmask;
        }
    } else if (fdcount == -1) {
        return FW_EVT_ERR;
    }

    return fdcount;
}
#elif defined(__linux__)
#error "LINUX not yet supported"
#endif
/* MAC OS implementation END - kqueue
 * ===========================================================================*/

/*============================================================================
 * GENERIC API
 *============================================================================*/

fwLoop *fwLoopNew(int eventcount, int timeout) {
    int eventssize;
    fwLoop *fwl;
    fwEvt *idle, *active;

    fwl = NULL;
    idle = active = NULL;

    if ((fwl = malloc(sizeof(fwLoop))) == NULL) {
        goto error;
    }

    if ((idle = malloc(sizeof(fwEvt) * eventcount)) == NULL) {
        goto error;
    }

    if ((active = malloc(sizeof(fwEvt) * eventcount)) == NULL) {
        goto error;
    }

    fwl->max = -1;
    fwl->count = eventcount;
    fwl->idle = idle;
    fwl->active = active;
    fwl->timeout = timeout;
    fwl->processed = 0;
    fwl->run = 1;
    for (int i = 0; i < eventcount; ++i) {
        fwl->idle[i].mask = FW_EVT_ADD;
    }
    if ((fwl->fd = fwStateCreate(fwl)) == FW_EVT_ERR) {
        goto error;
    }

    return fwl;

error:
    if (fwl) {
        free(fwl);
    }
    if (idle) {
        free(idle);
    }
    if (active) {
        free(active);
    }

    return NULL;
}

int fwLoopAddEvent(fwLoop *fwl, int fd, int mask, fwEvtCallback *cb,
                   void *data) {
    fwEvt *ev;

    if (fd >= fwl->count) {
        return FW_EVT_ERR;
    }

    ev = &fwl->idle[fd];
    if (fwStateAdd(fwl, fd, mask) == FW_EVT_ERR) {
        return FW_EVT_ERR;
    }

    ev->mask |= mask;
    ev->data = data;
    if (mask & FW_EVT_WATCH) {
        ev->watch = cb;
    }
    if (fd > fwl->max) {
        fwl->max = fd;
    }

    return FW_EVT_OK;
}

void fwLoopDeleteEvent(fwLoop *fwl, int fd, int mask) {
    fwEvt *ev;
    int i;

    if (fd >= fwl->count) {
        return;
    }

    ev = &fwl->idle[fd];

    if (ev->mask == FW_EVT_ADD) {
        return;
    }

    fwStateDelete(fwl, fd, mask);
    ev->mask = ev->mask & (~mask);
    if (fd == fwl->max && ev->mask == FW_EVT_ADD) {
        for (i = fwl->max - 1; i >= 0; --i) {
            if (fwl->idle[i].mask != FW_EVT_ADD) {
                break;
            }
        }
        fwl->max = i;
    }
}

size_t fwLoopGetProcessedEventCount(fwLoop *fwl) {
    return fwl->processed;
}

void fwLoopStop(fwLoop *fwl) {
    fwl->run = 0;
}

void fwLoopProcessEvents(fwLoop *fwl) {
    int eventcount;

    if (fwl->max == -1) {
        return;
    }

    if ((eventcount = fwPoll(fwl)) == FW_EVT_ERR) {
        return;
    }

    for (int i = 0; i < eventcount; ++i) {
        int fd = fwl->active[i].fd;
        fwEvt *ev = &fwl->idle[fd];
        int mask = fwl->active[i].mask;

        if (mask & FW_EVT_WATCH || mask & FW_EVT_DELETE) {
            fwEvtWatch(ev, fwl, fd, mask);
        }

        fwl->processed++;
    }
}

void fwLoopMain(fwLoop *fwl) {
    fwl->run = 1;
    while (fwl->run) {
        fwLoopProcessEvents(fwl);
    }
}
