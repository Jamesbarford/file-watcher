#include <sys/types.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw.h"

/* Events to fire for an event, this is non exhaustive */
typedef struct fwEvt {
    int fd;
    /* Events mask */
    int mask;
    /* Callback to invoke for the watched file */
    fwEvtCallback *watch;
    /* User data */
    void *data;
} fwEvt;

/* the loop itself */
typedef struct fwLoop {
    /* highest number filedescriptor we have seen thus far */
    int max;
    /* Maximum number of events */
    int max_events;
    /* Optional timeout for the event loop */
    int timeout;
    /* Event loop filedescriptor */
    int fd;
    /* How many events have been processed */
    size_t processed;
    /* Control start / stopping of the event loop */
    int run;
    fwEvt *idle;
    /* Events ready */
    fwEvt *active;
    void *state; /* Allow for generic implementation of state */
} fwLoop;

typedef struct fwFile {
    /* Filedescriptor */
    int fd;
    /* how big the file is */
    long long size;
    /* file last updated time */
    time_t last_update;
    /* Name of the file */
    char *name;
} fwFile;

typedef struct fwState {
    /* How much memory we have for fws */
    size_t capacity;
    /* How many files we are tracking in fws */
    size_t count;
    /* Maximum number of files we can track */
    int max_open;
    /* Command to repeatedly run */
    char *command;
    /* Array of files */
    fwFile *fws;
    /* Event loop for watching the files */
    fwLoop *fwl;
} fwState;

static char *command = NULL;
static pid_t child_p = -1;

#if defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6) || \
        defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define IS_BSD (1)
#elif defined(__linux__)
#define IS_LINUX (1)
#endif

#define fwPanic(...)                                                   \
    do {                                                               \
        fprintf(stderr, "! %s:%d:%s  ", __FILE__, __LINE__, __func__); \
        fprintf(stderr, __VA_ARGS__);                                  \
        exit(EXIT_FAILURE);                                            \
    } while (0)

#define fwWarn(...)                                                    \
    do {                                                               \
        fprintf(stderr, "- %s:%d:%s  ", __FILE__, __LINE__, __func__); \
        fprintf(stderr, __VA_ARGS__);                                  \
    } while (0)

#define fwInfo(...)                                                    \
    do {                                                               \
        fprintf(stderr, "+ %s:%d:%s  ", __FILE__, __LINE__, __func__); \
        fprintf(stderr, __VA_ARGS__);                                  \
    } while (0)

#ifdef DEBUG
#define fwDebug(...)                                                   \
    do {                                                               \
        fprintf(stderr, "[DEBUG] %s:%d:%s  ", __FILE__, __LINE__,      \
                __func__);                                             \
        fprintf(stderr, __VA_ARGS__);                                  \
    } while (0)
#else
#define fwDebug(...)
#endif


#if defined(IS_BSD)
#define statFileUpdated(sb) (sb.st_mtime)
#define statFileCreated(sb) (sb.st_birthtime)
#define OPEN_FILE_FLAGS     (O_RDONLY|O_EVTONLY)
#elif defined(IS_LINUX)
#define statFileUpdated(sb)   (sb.st_mtim.tv_sec)
#define ststatFileCreated(sb) (sb.st_ctim.tv_sec)
#define OPEN_FILE_FLAGS       (O_RDONLY)
#else
#error "Cannot determine how to get information time from 'struct stat'"
#endif

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

static int fwLoopStateNew(fwLoop *fwl) {
    fwEvtState *es;

    if ((es = malloc(sizeof(fwEvtState))) == NULL) {
        goto error;
    }

    if ((es->events = malloc(sizeof(struct kevent) * fwl->max_events)) ==
        NULL) {
        goto error;
    }

    if ((es->kfd = kqueue()) == -1) {
        fwDebug("Failed to invoke kqueue(): %s\n", strerror(errno));
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

static void fwLoopStateRelease(fwLoop *fwl) {
    if (fwl) {
        fwEvtState *es = fwLoopGetState(fwl);
        if (es) {
            close(es->kfd);
            free(es->events);
            free(es);
        }
    }
}

/* Add a filedescriptor for kqueue to watch */
static int fwLoopStateAdd(fwLoop *fwl, int fd, int mask) {
    fwEvtState *es = fwLoopGetState(fwl);
    struct kevent change;

    /* WATCH is the most generic one, adding everything for now */
    if (mask & FW_EVT_WATCH) {
        EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB, 0, 0);
        fwDebug("%d\n", change.ident);
        if (kevent(es->kfd, &change, 1, NULL, 0, NULL) == -1) {
            fwDebug("Failed to add: %d to queue: %s\n", fd, strerror(errno));
            return FW_EVT_ERR;
        }
    }
    return FW_EVT_OK;
}

static void fwLoopStateDelete(fwLoop *fwl, int fd, int mask) {
    fwEvtState *es = fwl->state;
    struct kevent event;

    /* Remove this event for the kqueue */
    EV_SET(&event, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    __kevent(es->kfd, &event);
}

/* Check for activity on a file descriptor */
static int fwLoopPoll(fwLoop *fwl) {
    fwEvtState *es = fwLoopGetState(fwl);
    int fdcount = 0;

    fdcount = kevent(es->kfd, NULL, 0, es->events, fwl->max_events, NULL);

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
#include <sys/epoll.h>
#include <sys/inotify.h>

#define EVENT_SIZE    (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

typedef struct fwEvtState {
    int ifd;
    int epollfd;
    struct epoll_event *events;
    struct epoll_event *ev;
} fwEvtState;

static int fwLoopStateNew(fwLoop *fwl) {
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
    if (epoll_ctl(es->epollfd, EPOLL_CTL_ADD, es->ifd, es->ev) == -1) {
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

/* https://stackoverflow.com/questions/16760364/using-inotify-why-is-my-watched-file-ignored
 */
static int fwLoopStateAdd(fwLoop *fwl, int fd, int mask) {
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
        flags |= IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_MODIFY |
                IN_ATTRIB;
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

static void fwLoopStateDelete(fwLoop *fwl, int wfd, int mask) {
    fwEvtState *es = fwLoopGetState(fwl);
    /* Get rid of file from watched files, this may well error */
    (void)inotify_rm_watch(es->ifd, wfd);
}

static void fwLoopStateRelease(fwLoop *fwl) {
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

static int fwLoopPoll(fwLoop *fwl) {
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
                event = (struct inotify_event *)&event_buffer[i];
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
                        evt->mask = FW_EVT_WATCH | FW_EVT_DELETE;

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
    if ((fwl->fd = fwLoopStateNew(fwl)) == FW_EVT_ERR) {
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

    fwDebug("Failed to create eventloop\n");
    return NULL;
}

int fwLoopAddEvent(fwLoop *fwl, int fd, int mask, fwEvtCallback *cb,
                   void *data) {
    fwEvt *ev;

    if (fd >= fwl->max_events) {
        return FW_EVT_ERR;
    }

#if defined(IS_BSD)
    fwDebug(">>%d\n", fd);
    if (fwLoopStateAdd(fwl, fd, mask) == FW_EVT_ERR) {
        return FW_EVT_ERR;
    }
    ev = &fwl->idle[fd];
#endif

/* We do not need the filedescriptor as inotify uses it's own filedescriptors */
#if defined(IS_LINUX)
    int wfd = 0;
    if ((wfd = fwLoopStateAdd(fwl, fd, mask)) == FW_EVT_ERR) {
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
    fwDebug("HERE\n");
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

    fwLoopStateDelete(fwl, fd, mask);
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

/* Kill the child process running the command */
static void fwSigtermHandler(int sig) {
    kill(child_p, SIGTERM);
    exit(EXIT_SUCCESS);
}

/* The dynamic array for storing file state */
fwState *fwStateNew(char *command, int max_open, int timeout) {
    struct sigaction act;
    fwLoop *fwl;
    fwState *ws;

    if ((ws = malloc(sizeof(fwState))) == NULL) {
        return NULL;
    }
    if ((ws->fws = malloc(sizeof(fwFile) * ws->capacity)) == NULL) {
        free(ws);
        return NULL;
    }

    ws->count = 0;
    ws->capacity = 10;
    ws->command = command;
    ws->max_open = max_open;
    ws->fwl = fwLoopNew(ws->max_open, timeout);

    act.sa_handler = fwSigtermHandler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, NULL);

    return ws;
}

void fwStateRelease(fwState *ws) {
    if (ws) {
        for (int i = 0; i < ws->count; ++i) {
            free(ws->fws[i].name);
            close(ws->fws[i].fd);
        }
        fwLoopStateRelease(ws->fwl);
        free(ws->fws);
        free(ws);
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

    if ((eventcount = fwLoopPoll(fwl)) == FW_EVT_ERR) {
        return;
    }

    for (int i = 0; i < eventcount; ++i) {
        int fd = fwl->active[i].fd;
        fwEvt *ev = &fwl->idle[fd];
        int mask = fwl->active[i].mask;

        /* IF some kind of event */
        if (mask) {
            fwEvtWatch(ev, fwl, fd, mask);
        }
        fwl->processed++;
    }
}

int fwAddFile(fwState *ws, char *file_name) {
    struct stat sb;
    int fd;
    char abspath[1048];

    if (ws->count >= ws->max_open) {
        fwWarn("Trying to add more than: %d files\n", ws->max_open);
        return -1;
    }

    if (ws->count >= ws->capacity) {
        ws->fws = realloc(ws->fws, (ws->capacity * 2) * sizeof(fwState));
        ws->capacity *= 2;
    }

    if ((fd = open(file_name, OPEN_FILE_FLAGS, 0644)) == -1) {
        fwDebug("CANNOT OPEN FILE: %s - %s\n", file_name, strerror(errno));
        return -1;
    }

    ws->fws[ws->count].fd = fd;

    fwDebug("OPENED: fd=%d\n",fd);

    if (fstat(ws->fws[ws->count].fd, &sb) == -1) {
        fwDebug("Failed to add to fstat: %s\n", strerror(errno));
        close(ws->fws[ws->count].fd);
        return -1;
    }

    if (realpath(file_name, abspath) == NULL) {
        fwDebug("Failed to add to realpath: %s\n", strerror(errno));
        close(ws->fws[ws->count].fd);
        return -1;
    }

    ws->fws[ws->count].last_update = statFileUpdated(sb);
    ws->fws[ws->count].name = strdup(abspath);

    ws->count++;
    return 0;
}

/* Add multiple files to the watch state */
void fwAddFiles(fwState *ws, int argc, ...) {
    va_list ap;
    char *file_name;
    va_start(ap, argc);
    for (int i = 0; i < argc; ++i) {
        file_name = va_arg(ap, char *);
        if (fwAddFile(ws, file_name) != 0) {
            break;
        }
    }
    va_end(ap);
}

/* Add a directory, this is not recursive */
int fwAddDirectory(fwState *ws, char *dirname, char *ext, int extlen) {
    DIR *dir = opendir(dirname);
    struct dirent *dr;
    struct stat sb;
    int files_open = 0, len = 0, should_add = 0;
    char full_path[1024];

    if (dir == NULL) {
        return -1;
    }

    while ((dr = readdir(dir)) != NULL && files_open < ws->max_open) {
        switch (dr->d_type) {
        case DT_REG:
            if (!strlen(dr->d_name) && dr->d_name[0] == '.' ||
                strlen(dr->d_name) == 2 && dr->d_name[0] == '.' &&
                        dr->d_name[1] == '.') {
                continue;
            }

            should_add = 1;
            len = snprintf(full_path, sizeof(full_path), "%s/%s", dirname,
                           dr->d_name);
            full_path[len] = '\0';
            if (ext) {
                for (int i = len - 1, j = extlen - 1; j; --i) {
                    if (full_path[i] != ext[j]) {
                        should_add = 0;
                        break;
                    }
                    j--;
                }
            }
            if (should_add) {
                printf("ADDING : %s\n ", full_path);
                fwAddFile(ws, full_path);
            }
            break;
        case DT_DIR:
            fwWarn("Directories not yet supported: %s\n", dr->d_name);
            break;
        default:
            break;
        }
    }
    return 0;
}

static void fwReRunCommand(void) {
    fwDebug("child_p: %d\n", child_p);
    /* Kill the previous session if required */
    if (child_p != -1) {
        fwDebug("child_p: %d\n", child_p);
        kill(child_p, SIGTERM);    // Use SIGTERM to allow child to cleanup
        waitpid(child_p, NULL, 0); // Reap the child process
        fwDebug("Parent: Child terminated\n");
    }

    if ((child_p = fork()) == 0) {
        system(command);
        exit(EXIT_SUCCESS); // Make sure to exit after the system call in child
    }
}

static void fwListener(fwLoop *fwl, int fd, void *data, int type) {
    fwFile *fw = (fwFile *)data;
    struct stat sb;

    if (type & (FW_EVT_DELETE | FW_EVT_WATCH)) {
        close(fw->fd);
        if (access(fw->name, F_OK) == -1 && errno == ENOENT) {
            fwDebug("DELETED: %s\n", fw->name);
            free(fw->name);
            free(fw);
            fwLoopDeleteEvent(fwl, fd, FW_EVT_WATCH);
            return;
        } else {
            fwLoopDeleteEvent(fwl, fd, FW_EVT_WATCH);
            fw->fd = open(fw->name, OPEN_FILE_FLAGS, 0644);
            fwLoopAddEvent(fwl, fw->fd, FW_EVT_WATCH, fwListener, fw);
        }

        if (stat(fw->name, &sb) == -1) {
            fwWarn("Could not update stats for file: %s\n", fw->name);
            return;
        }

        fw->size = sb.st_size;
        fw->last_update = statFileUpdated(sb);
        fwReRunCommand();
    }
}

void fwLoopMain(fwState *ws) {
    fwFile *fw;
    struct stat st;

    /* Add all files for watching */
    for (int i = 0; i < ws->count; ++i) {
        fw = &ws->fws[i];
        if(stat(fw->name, &st) == -1) {
            perror("Failed to stat");
            exit(1);
        }
        fw->size = st.st_size;
        fwDebug("%s %d %zu\n", fw->name, fw->fd, fw->size);
        if (fwLoopAddEvent(ws->fwl, fw->fd, FW_EVT_WATCH, fwListener, fw) == FW_EVT_ERR) {
            perror("Failed to add event: ");
            exit(1);
        }
    }

    /* Run the event loop */
    while (ws->fwl->run) {
        fwLoopProcessEvents(ws->fwl);
    }
}
