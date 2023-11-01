#include <sys/types.h>
#include <sys/epoll.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "osconfig.h"
#include "fw.h"

/* Events to fire for an event, this is non exhaustive */
typedef struct fwEvt {
    int fd;
    int mask;
    fwEvtCallback *watch;
    void *data;
} fwEvt;

/* the loop itself */
typedef struct fwLoop {
    int max;
    int max_events;
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
#if defined(IS_BSD)
#include <sys/event.h>

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
/* MAC OS implementation END - kqueue
 * ===========================================================================*/

#elif defined(IS_LINUX)
#include <sys/inotify.h>

#define EVENT_SIZE    (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

typedef struct fwEvtState {
    int ifd;
    int epollfd;
    struct epoll_event *events;
    struct epoll_event *ev;
} fwEvtState;

static int fwStateCreate(fwLoop *fwl) {
    fwEvtState *es;

    if ((es = malloc(sizeof(fwEvtState))) == NULL) {
        goto error;
    }
    
    if ((es->ev = malloc(sizeof(struct epoll_event))) == NULL) {
        goto error;
    }

    if ((es->events = malloc(sizeof(struct epoll_event) * fwl->max_events)) == NULL) {
        goto error;
    }

    if ((es->ifd = inotify_init()) == -1) {
        goto error;
    }

    if ((es->epollfd = epoll_create(EPOLL_CLOEXEC)) == -1) {
        goto error;
    }

    es->ev->events = EPOLLIN | EPOLLOUT | EPOLLET;
    es->ev->data.fd = es->ifd;
    if (epoll_ctl(es->epollfd, EPOLL_CTL_ADD, es->ifd, es->ev) == -1)  {
        goto error;
    }

    fwl->state = es;

    return FW_EVT_OK;
error:
    if (es) {
        free(es);
    }
    if (es->ev) {
        free(es->ev);
    }
    if (es->events) {
        free(es->events);
    }
    if (es->ifd != -1) {
        close(es->ifd);
    }
    return FW_EVT_ERR;
}

/* https://stackoverflow.com/questions/16760364/using-inotify-why-is-my-watched-file-ignored */
static int fwStateAdd(fwLoop *fwl, int fd, int mask) {
    int wfd, len;
    char abspath[1048], procpath[1048];
    fwEvtState *es = fwLoopGetState(fwl);
    pid_t pid;
    int flags = 0;
    
    if ((pid = getpid()) == -1) {
        return FW_EVT_ERR;
    }

    /* Getting the absolute file path from a file descriptor */
    len = snprintf(procpath, sizeof(procpath), "/proc/%d/fd/%d", pid, fd);
    procpath[len] = '\0'; 
   
    if ((len = readlink(procpath, abspath, sizeof(abspath))) == -1) {
        return FW_EVT_ERR;
    }
    abspath[len] = '\0';
    
    if (mask & FW_EVT_DELETE) {
        flags |= IN_DELETE | IN_DELETE_SELF | IN_ATTRIB;
    }

    if (mask & FW_EVT_WATCH) {
        flags |= IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_MODIFY | IN_ATTRIB;
    }

    if (mask & FW_EVT_MOVE) {
        flags |= IN_MOVE;
    }

    if (mask & FW_EVT_OPEN) {
        flags |= IN_OPEN;
    }

    if (mask & FW_EVT_CLOSE) {
        flags |= IN_CLOSE;
    }

    if ((wfd = inotify_add_watch(es->ifd, abspath, flags)) == -1) {
        return FW_EVT_ERR;
    }
    close(fd);

    printf("added ok: %d\n", wfd);
    return wfd;
}

static void fwStateDelete(fwLoop *fwl, int wfd, int mask) {
    fwEvtState *es = fwLoopGetState(fwl);
    /* Get rid of file from watched files, this may well error */
    (void)inotify_rm_watch(es->ifd, wfd);
}

static void fwStateRelease(fwLoop *fwl) {
    if (fwl) {
        fwEvtState *es = fwLoopGetState(fwl);
        if (es) {
            epoll_ctl(es->epollfd, EPOLL_CTL_DEL, es->ifd, es->ev);
            free(es->events);
            free(es->ev);
            close(es->epollfd);
            close(es->ifd);
            free(es);
        }
    }
}

static int fwPoll(fwLoop *fwl) {
    fwEvtState *es = fwLoopGetState(fwl);
    fwEvt *evt;
    struct inotify_event *event;
    struct epoll_event *change;
    char event_buffer[EVENT_BUF_LEN];
    int fdcount = epoll_wait(es->epollfd, es->events, fwl->max_events, -1);
    int event_count = 0;
    int j = 0;
    int cur_wfd = -1;

    if (fdcount == -1) {
        return FW_EVT_ERR;
    }

    if (fdcount > 0) {
        for (int i = 0; i < fdcount; ++i) {
            change = &es->events[i];

            if ((event_count = read(es->ifd, event_buffer, EVENT_BUF_LEN)) <= 0) {
                return FW_EVT_ERR;
            }

            while (i < event_count) {
                event = (struct inotify_event *) &event_buffer[i];
                cur_wfd = event->wd;
                evt = &fwl->active[j];
                evt->fd = event->wd;
                
                if (cur_wfd == -1) {
                    cur_wfd = event->wd;
                    j = 0;
                } else {
                    if (cur_wfd != event->wd) {
                        j++;
                    }
                }
                
                if (event != NULL) {
                    if (event->mask & IN_CREATE) {
                        evt->mask = FW_EVT_CREATE;
                    
                    } else if (event->mask & IN_DELETE) {
                        evt->mask = FW_EVT_DELETE;
                    
                    } else if (event->mask & IN_MODIFY) {
                        evt->mask = FW_EVT_WATCH;
                    
                    } else if (event->mask & IN_IGNORED) {
                        evt->mask = FW_EVT_WATCH|FW_EVT_DELETE;
                    
                    } else if (event->mask & IN_OPEN) {
                        evt->mask = FW_EVT_OPEN; 
                    
                    } else if (event->mask & IN_DELETE_SELF) {
                        evt->mask = FW_EVT_DELETE; 

                    } else if (event->mask & IN_MOVE_SELF) {
                        evt->mask = FW_EVT_MOVE; 

                    } else if (event->mask & IN_ATTRIB) {
                        evt->mask = FW_EVT_WATCH;

                    } else if (event->mask & IN_CLOSE) {
                        evt->mask = FW_EVT_CLOSE;
                    } 
                }
                i += EVENT_SIZE + event->len;
            }
        }
    } else if (fdcount == -1) {
        return FW_EVT_ERR;
    }

    return fdcount;
}

#endif

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
    fwl->max_events = eventcount;
    fwl->idle = idle;
    fwl->active = active;
    fwl->timeout = timeout;
    fwl->processed = 0;
    fwl->run = 1;
    for (int i = 0; i < fwl->max_events; ++i) {
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

    if (fd >= fwl->max_events) {
        return FW_EVT_ERR;
    }

#if defined(IS_BSD)
    if (fwStateAdd(fwl, fd, mask) == FW_EVT_ERR) {
        return FW_EVT_ERR;
    }
    ev = &fwl->idle[fd];
#endif

/* We do not need the filedescriptor as inotify uses it's own filedescriptors */
#if defined (IS_LINUX)
    int wfd = 0;
    if ((wfd = fwStateAdd(fwl, fd, mask)) == FW_EVT_ERR) {
        return FW_EVT_ERR;
    }
    ev = &fwl->idle[wfd];
    ev->fd = fd;
#endif

    ev->mask |= mask;
    ev->data = data;
    ev->watch = cb;

    if (fd > fwl->max) {
        fwl->max = fd;
    }
    return FW_EVT_OK;
}

void fwLoopDeleteEvent(fwLoop *fwl, int fd, int mask) {
    fwEvt *ev;
    int i;

    if (fd >= fwl->max_events) {
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
        
        fwEvtWatch(ev, fwl, fd, mask);
        fwl->processed++;
    }
}

void fwLoopMain(fwLoop *fwl) {
    fwl->run = 1;
    while (fwl->run) {
        fwLoopProcessEvents(fwl);
    }
}
