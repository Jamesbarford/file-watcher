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
    /* Maximum number of files we can track */
    int max_events;
    /* Command to repeatedly run for this context */
    char *command;
    /* How many events have been processed */
    size_t processed_events;
    /* 1 = run event loop, 0 = stop */
    int run_loop;
    /* Current highest open filedescriptor */
    int fd_current_max;
    /* How long to poll for, set to -1 to never stop */
    int poll_timeout;
    /* How many files we are tracking in fws */
    size_t files_count;
    /* How much memory we have for files array */
    size_t files_mem_capacity;
    /* Array of files */
    fwFile *files_array;
    /* Events that are idle */
    fwEvt *idle;
    /* Events ready */
    fwEvt *active;
    /* Allow for OS specific implementation */
    void *evt_state; 
} fwState;

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


#ifdef DEBUG
#define fwWarn(...)                                                    \
    do {                                                               \
        fprintf(stderr, "- %s:%d:%s  ", __FILE__, __LINE__, __func__); \
        fprintf(stderr, __VA_ARGS__);                                  \
    } while (0)

#define fwDebug(...)                                                   \
    do {                                                               \
        fprintf(stderr, "[DEBUG] %s:%d:%s  ", __FILE__, __LINE__,      \
                __func__);                                             \
        fprintf(stderr, __VA_ARGS__);                                  \
    } while (0)
#else
#define fwDebug(...)
#define fwWarn(...)
#endif


#if defined(IS_BSD)
#define statFileUpdated(sb) (sb.st_mtime)
#define statFileCreated(sb) (sb.st_birthtime)
#define OPEN_FILE_FLAGS     (O_RDONLY)
#elif defined(IS_LINUX)
#define statFileUpdated(sb)   (sb.st_mtim.tv_sec)
#define ststatFileCreated(sb) (sb.st_ctim.tv_sec)
#define OPEN_FILE_FLAGS       (O_RDONLY)
#else
#error "Cannot determine how to get information time from 'struct stat'"
#endif

#define fwLoopGetEvtState(fwl) ((fwl)->evt_state)

#define fwEvtWatch(ev, fws, fd, mask) \
    ((ev)->watch((fws), (fd), (ev)->data, (mask)))

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

static fwEvtState *fwLoopStateNew(int max_events) {
    fwEvtState *es;

    if ((es = malloc(sizeof(fwEvtState))) == NULL) {
        goto error;
    }

    if ((es->events = malloc(sizeof(struct kevent) * max_events)) == NULL) {
        goto error;
    }

    if ((es->kfd = kqueue()) == -1) {
        fwDebug("Failed to invoke kqueue(): %s\n", strerror(errno));
        goto error;
    }
    fwDebug("kqueue() fd=%d\n", es->kfd);

    return es;

error:
    if (es && es->events) {
        free(es->events);
    }
    if (es) {
        free(es);
    }
    return NULL;
}

/* Add a filedescriptor for kqueue to watch */
static int fwLoopStateAdd(fwState *fws, int fd, int mask) {
    fwEvtState *es = fwLoopGetEvtState(fws);
    struct kevent change;

    /* WATCH is the most generic one, adding everything for now */
    if (mask & FW_EVT_WATCH) {
        EV_SET(&change, fd, EVFILT_VNODE, EV_ADD,
               NOTE_WRITE | NOTE_FUNLOCK | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB, 0, 0);
        if (kevent(es->kfd, &change, 1, NULL, 0, NULL) == -1) {
            return FW_EVT_ERR;
        }
    }
    return FW_EVT_OK;
}

static void fwLoopStateDelete(fwState *fws, int fd, int mask) {
    fwEvtState *es = fws->evt_state;
    struct kevent event;

    /* Remove this event for the kqueue */
    EV_SET(&event, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    __kevent(es->kfd, &event);
}

/* Check for activity on a file descriptor */
static int fwLoopPoll(fwState *fws) {
    fwEvtState *es = fwLoopGetEvtState(fws);
    int fdcount = 0;

    fdcount = kevent(es->kfd, NULL, 0, es->events, fws->max_events, NULL);

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

            fws->active[i].fd = change->ident;
            fws->active[i].mask = newmask;
        }
    } else if (fdcount == -1) {
        return FW_EVT_ERR;
    }

    return fdcount;
}

static void fwEvtStateRelease(fwState *fws) {
    fwEvtState *es = fwLoopGetEvtState(fws);
    if (es) {
        close(es->kfd);
        free(es->events);
        free(es);
    }
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

static fwEvtState *fwLoopStateNew(int max_events) {
    fwEvtState *es;

    if ((es = malloc(sizeof(fwEvtState))) == NULL) {
        goto error;
    }

    if ((es->ev = malloc(sizeof(struct epoll_event))) == NULL) {
        goto error;
    }

    if ((es->events = malloc(sizeof(struct epoll_event) * max_events)) == NULL) {
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


    return es;
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
    return NULL;
}

/* https://stackoverflow.com/questions/16760364/using-inotify-why-is-my-watched-file-ignored
 */
static int fwLoopStateAdd(fwState *fws, int fd, int mask) {
    int wfd, len;
    char abspath[1048], procpath[1048];
    fwEvtState *es = fwLoopGetEvtState(fws);
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

    return wfd;
}

static void fwLoopStateDelete(fwState *fws, int wfd, int mask) {
    fwEvtState *es = fwLoopGetEvtState(fws);
    /* Get rid of file from watched files, this may well error */
    (void)inotify_rm_watch(es->ifd, wfd);
}

static void fwEvtStateRelease(fwState *fws) {
    if (fws) {
        fwEvtState *es = fwLoopGetEvtState(fws);
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

static int fwLoopPoll(fwState *fws) {
    fwEvtState *es = fwLoopGetEvtState(fws);
    fwEvt *evt;
    struct inotify_event *event;
    struct epoll_event *change;
    char event_buffer[EVENT_BUF_LEN];
    int fdcount = epoll_wait(es->epollfd, es->events, fws->max_events, -1);
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
                evt = &fws->active[j];
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

int fwLoopAddEvent(fwState *fws, int fd, int mask, fwEvtCallback *cb,
                   void *data) {
    fwEvt *ev;

    if (fd >= fws->max_events) {
        return FW_EVT_ERR;
    }

#if defined(IS_BSD)
    fwDebug(">>%d\n", fd);
    if (fwLoopStateAdd(fws, fd, mask) == FW_EVT_ERR) {
        return FW_EVT_ERR;
    }
    ev = &fws->idle[fd];
#endif

/* We do not need the filedescriptor as inotify uses it's own filedescriptors */
#if defined(IS_LINUX)
    int wfd = 0;
    if ((wfd = fwLoopStateAdd(fws, fd, mask)) == FW_EVT_ERR) {
        return FW_EVT_ERR;
    }
    ev = &fws->idle[wfd];
    ev->fd = fd;
#endif

    ev->mask |= mask;
    ev->data = data;
    ev->watch = cb;

    if (fd > fws->fd_current_max) {
        fws->fd_current_max = fd;
    }
    fwDebug("HERE\n");
    return FW_EVT_OK;
}

void fwLoopDeleteEvent(fwState *fws, int fd, int mask) {
    fwEvt *ev;
    int i;

    if (fd >= fws->max_events) {
        return;
    }

    ev = &fws->idle[fd];

    if (ev->mask == FW_EVT_ADD) {
        return;
    }

    fwLoopStateDelete(fws, fd, mask);
    ev->mask = ev->mask & (~mask);
    if (fd == fws->fd_current_max && ev->mask == FW_EVT_ADD) {
        for (i = fws->fd_current_max - 1; i >= 0; --i) {
            if (fws->idle[i].mask != FW_EVT_ADD) {
                break;
            }
        }
        fws->fd_current_max = i;
    }
}

/* Kill the child process running the command */
static void fwSigtermHandler(int sig) {
    if (child_p != -1) {
        kill(child_p, SIGTERM);
    }
    exit(EXIT_SUCCESS);
}

/* The dynamic array for storing file state */
fwState *fwStateNew(char *command, int max_events, int timeout) {
    struct sigaction act;
    fwState *fws;

    if ((fws = malloc(sizeof(fwState))) == NULL) {
        goto error;
    }

    fws->files_array = NULL;
    fws->idle = NULL;
    fws->active = NULL;
    fws->evt_state = NULL;

    if ((fws->files_array = malloc(sizeof(fwFile) * 10)) == NULL) {
        goto error;
    }

    if ((fws->idle = malloc(sizeof(fwEvt) * max_events)) == NULL) {
        goto error;
    }

    if ((fws->active = malloc(sizeof(fwEvt) * max_events)) == NULL) {
        goto error;
    }

    if ((fws->evt_state = fwLoopStateNew(max_events)) == NULL) {
        goto error;
    }

    fws->files_count = 0;
    fws->files_mem_capacity = 10;
    fws->command = strdup(command);
    fws->max_events = max_events;
    fws->fd_current_max = -1;
    fws->poll_timeout = timeout;
    fws->processed_events = 0;
    fws->run_loop = 1;
    act.sa_handler = fwSigtermHandler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, NULL);

    for (int i = 0; i < fws->max_events; ++i) {
        fws->idle[i].mask = FW_EVT_ADD;
    }
    fwDebug("Pre CREATE LOOP STATE\n");


    return fws;

error:
    fwDebug("Failed to create eventloop\n");
    if (fws->idle) {
        free(fws->idle);
    }
    if (fws->active) {
        free(fws->active);
    }
    if (fws->evt_state) {
        free(fws->evt_state);
    }
    if (fws) {
        free(fws);
    }

    return NULL;


    return fws;
}

/* Destroy the event loop and OS specific event state. Closes all open file 
 * descriptors, names of files and command */
void fwStateRelease(fwState *fws) {
    if (fws) {
        for (int i = 0; i < fws->files_count; ++i) {
            free(fws->files_array[i].name);
            close(fws->files_array[i].fd);
        }
        free(fws->files_array);
        free(fws->command);
        fwEvtStateRelease(fws);
        free(fws);
    }
}

size_t fwLoopGetProcessedEventCount(fwState *fws) {
    return fws->processed_events;
}

void fwLoopStop(fwState *fws) {
    fws->run_loop = 0;
}

void fwLoopProcessEvents(fwState *fws) {
    int eventcount;

    if (fws->fd_current_max == -1) {
        return;
    }

    if ((eventcount = fwLoopPoll(fws)) == FW_EVT_ERR) {
        return;
    }

    for (int i = 0; i < eventcount; ++i) {
        int fd = fws->active[i].fd;
        fwEvt *ev = &fws->idle[fd];
        int mask = fws->active[i].mask;

        /* If some kind of event that the user has subscribed to
         * TODO: maintain user defined flags? Although we make a best effort
         * to map our flags to the OS types */
        if (mask) {
            ev->watch(fws, fd, ev->data, mask);
        }
        fws->processed_events++;
    }
}

static void fwRunCommand(char *command) {
    /* Kill the previous session if required */
    if (child_p != -1) {
        fwDebug("child_p: %d\n", child_p);
        kill(child_p, SIGTERM);    // Use SIGTERM to allow child to cleanup
        waitpid(child_p, NULL, 0); // Reap the child process
        fwDebug("Parent: Child terminated\n");
    }

    if ((child_p = fork()) == 0) {
        fwDebug("Running command\n");
        system(command);
        exit(EXIT_SUCCESS); // Make sure to exit after the system call in child
    }
}

static void fwListener(fwState *fws, int fd, void *data, int type) {
    fwFile *fw = (fwFile *)data;
    struct stat sb;

    if (type & (FW_EVT_DELETE | FW_EVT_WATCH)) {
        close(fw->fd);
        if (access(fw->name, F_OK) == -1 && errno == ENOENT) {
            fwDebug("DELETED: %s\n", fw->name);
            free(fw->name);
            free(fw);
            fwLoopDeleteEvent(fws, fd, FW_EVT_WATCH);
            return;
        } else {
            fwLoopDeleteEvent(fws, fd, FW_EVT_WATCH);
            fw->fd = open(fw->name, OPEN_FILE_FLAGS, 0644);
            fwLoopAddEvent(fws, fw->fd, FW_EVT_WATCH, fwListener, fw);
        }

        if (stat(fw->name, &sb) == -1) {
            fwWarn("Could not update stats for file: %s\n", fw->name);
            return;
        }

        fw->size = sb.st_size;
        fw->last_update = statFileUpdated(sb);
        fwRunCommand(fws->command);
    }
}

int fwAddFile(fwState *ws, char *file_name) {
    struct stat sb;
    int fd;
    char abspath[1048];

    if (ws->files_count >= ws->max_events) {
        fwWarn("Trying to add more than: %d files\n", ws->max_events);
        return -1;
    }

    if (ws->files_count >= ws->files_mem_capacity) {
        ws->files_array = realloc(ws->files_array, (ws->files_mem_capacity * 2) * sizeof(fwState));
        ws->files_mem_capacity *= 2;
    }

    if ((fd = open(file_name, OPEN_FILE_FLAGS, 0644)) == -1) {
        fwDebug("CANNOT OPEN FILE: %s - %s\n", file_name, strerror(errno));
        return -1;
    }


    if (fstat(fd, &sb) == -1) {
        fwDebug("Failed to add to fstat: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Get the absolute filepath of the file on disk */
    if (realpath(file_name, abspath) == NULL) {
        fwDebug("Failed to add to realpath: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    ws->files_array[ws->files_count].fd = fd;
    ws->files_array[ws->files_count].last_update = statFileUpdated(sb);
    ws->files_array[ws->files_count].size = sb.st_size;
    ws->files_array[ws->files_count].name = strdup(abspath);

    fwEvtState *evt = ws->evt_state;

    /* Add the file to the watch list */
    if (fwLoopAddEvent(ws, ws->files_array[ws->files_count].fd, FW_EVT_WATCH, fwListener, &ws->files_array[ws->files_count]) == FW_EVT_ERR) {
        fwDebug("Failed to add event: filename=%s fd=%d reason: %s\n", file_name, fd, strerror(errno));
        exit(1);
    }

    ws->files_count++;
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

    while ((dr = readdir(dir)) != NULL && files_open < ws->max_events) {
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
                fwDebug("ADDING : %s\n ", full_path);
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
    closedir(dir);
    return 0;
}

void fwLoopMain(fwState *fws) {
    /* Run the event loop */
    while (fws->run_loop) {
        fwLoopProcessEvents(fws);
    }
}
