#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw.h"
#include "osconfig.h"

typedef struct watchedFile {
    int fd;
    long long size;
    time_t last_update;
    char *name;
} watchedFile;

typedef struct watchState {
    size_t capacity;
    size_t count;
    int max_open;
    char *command;
    watchedFile *fws;
} watchState;


static char *command = NULL;
static pid_t child_p = -1;

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

watchState *fwStateNew(char *command, int max_open) {
    watchState *ws = malloc(sizeof(watchState));
    ws->count = 0;
    ws->capacity = 10;
    ws->command = command;
    ws->max_open = max_open;
    ws->fws = malloc(sizeof(watchedFile) * ws->capacity);
    return ws;
}

void fwStateRelease(watchState *ws) {
    if (ws) {
        for (int i = 0; i < ws->count; ++i) {
            free(ws->fws[i].name);
            close(ws->fws[i].fd);
        }
        free(ws->fws);
        free(ws);
    }
}

int watchStateAddFile(watchState *ws, char *file_name) {
    struct stat sb;
    int fd;
    char abspath[1048];

    if (ws->count >= ws->max_open) {
        fwWarn("Trying to add more than: %d files\n", ws->max_open);
        return -1;
    }

    if (ws->count >= ws->capacity) {
        ws->fws = realloc(ws->fws, (ws->capacity * 2) * sizeof(watchState));
        ws->capacity *= 2;
    }

    if ((fd = open(file_name, OPEN_FILE_FLAGS, 0644)) == -1) {
        return -1;
    }

    ws->fws[ws->count].fd = fd;

    if (fstat(ws->fws[ws->count].fd, &sb) == -1) {
        close(ws->fws[ws->count].fd);
        return -1;
    }

    if (realpath(file_name, abspath) == NULL) {
        close(ws->fws[ws->count].fd);
        return -1;
    }

    ws->fws[ws->count].last_update = statFileUpdated(sb);
    ws->fws[ws->count].name = strdup(abspath);

    ws->count++;
    return 0;
}

void watchStateAddFiles(watchState *ws, int argc, ...) {
    va_list ap;
    char *file_name;
    va_start(ap, argc);

    for (int i = 0; i < argc; ++i) {
        file_name = va_arg(ap, char *);
        if (watchStateAddFile(ws, file_name) != 0) {
            break;
        }
    }

    va_end(ap);
}

int watchStateAddDirectory(watchState *ws, char *dirname, char *ext,
                           int extlen) {
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
                watchStateAddFile(ws, full_path);
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

void watcherReRunCommand(void) {
    /* Kill the previous session if required */
    if (child_p != -1) {
        printf("child_p: %d\n", child_p);
        kill(child_p, SIGTERM);    // Use SIGTERM to allow child to cleanup
        waitpid(child_p, NULL, 0); // Reap the child process
        printf("Parent: Child terminated\n");
    }

    if ((child_p = fork()) == 0) {
        system(command);
        exit(EXIT_SUCCESS); // Make sure to exit after the system call in child
    }
}

void watchFileListener(fwLoop *fwl, int fd, void *data, int type) {
    watchedFile *fw = (watchedFile *)data;
    struct stat sb;

    if (type & (FW_EVT_DELETE | FW_EVT_WATCH)) {
        close(fw->fd);
        if (access(fw->name, F_OK) == -1 && errno == ENOENT) {
            printf("DELETED: %s\n", fw->name);
            free(fw->name);
            free(fw);
            fwLoopDeleteEvent(fwl, fd, FW_EVT_WATCH);
            return;
        } else {
            fwLoopDeleteEvent(fwl, fd, FW_EVT_WATCH);
            fw->fd = open(fw->name, OPEN_FILE_FLAGS, 0644);
            fwLoopAddEvent(fwl, fw->fd, FW_EVT_WATCH, watchFileListener, fw);
        }

        if (stat(fw->name, &sb) == -1) {
            fwWarn("Could not update stats for file: %s\n", fw->name);
            return;
        }

        fw->size = sb.st_size;
        fw->last_update = statFileUpdated(sb);
        watcherReRunCommand();
    }
}

void watchForChanges(watchState *ws) {
    fwLoop *evt_loop = fwLoopNew(100, -1);

    for (int i = 0; i < ws->count; ++i) {
        watchedFile *fw = &ws->fws[i];
        struct stat st;
        fstat(fw->fd, &st);
        fw->size = st.st_size;
        if (fwLoopAddEvent(evt_loop, fw->fd, FW_EVT_WATCH, watchFileListener,
                           fw) == FW_EVT_ERR) {
            perror("??\n");
            exit(1);
        }
    }

    printf("starting loop\n");
    fwLoopMain(evt_loop);
    for (int i = 0; i < ws->count; ++i) {
        fwLoopDeleteEvent(evt_loop, ws->fws[i].fd, FW_EVT_WATCH);
    }
    fwStateRelease(ws);
}

void fwSigtermHandler(int sig) {
    printf("shutdown \n");
    kill(child_p, SIGTERM);
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fwPanic("Usage: %s <cmd> <dir>\n", argv[0]);
    }

    struct sigaction act;
    act.sa_handler = fwSigtermHandler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, NULL);

    command = argv[1];
    char *dirname = argv[2];
    watchState *ws = fwStateNew(command, INT_MAX);

    // watchStateAddDirectory(ws, dirname, ".c", 2);
    // watchStateAddDirectory(ws, dirname, ".h", 2);
    watchStateAddFile(ws, "./sample-files/foo.txt");

    if (ws->count == 0) {
        fwPanic("Failed to open all files\n");
    }
    printf("WATCHING: ");
    for (int i = 0; i < ws->count; ++i) {
        printf("%s ", ws->fws[i].name);
    }
    printf("\n");
    watchForChanges(ws);
    return EXIT_SUCCESS;
}
