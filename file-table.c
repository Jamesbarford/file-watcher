#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct fileEntry {
    int fd;
    char *name;
    int name_len;
    struct fileEntry *next;
} fileEntry;

typedef struct fileTable {
    int size;
    int capacity;
    int mask;
    fileEntry **entries;
} fileTable;

fileTable *fileTableNew(void) {
    fileTable *ft = malloc(sizeof(fileTable));
    ft->capacity = 16;
    ft->mask = ft->capacity - 1;
    ft->size = 0;
    ft->entries = malloc(sizeof(fileEntry * ) * ft->capacity);
    return ft;
}

int fileTableHas(fileTable *ft, int fd) {
    if (fd == -1) {
        return 0;
    }

    unsigned int hash_idx = (unsigned int)(fd & ft->capacity);
    fileEntry *fe = ft->entries[hash_idx]; 
    while (fe) {
        if (fe->fd == fd) {
            return 1;
        }
        fe = fe->next;
    }
    return 0;
}

int fileTableAdd(fileTable *ft, int fd, char *name, int name_len) {
    if (fileTableHas(ft, fd)) {
        return 0;
    }
    unsigned int hash_idx = (unsigned int)(fd & ft->capacity);
    fileEntry *newfe = malloc(sizeof(fileEntry));
    newfe->fd = fd;
    newfe->name = name;
    newfe->name_len = name_len;
    newfe->next = ft->entries[hash_idx];
    ft->entries[hash_idx] = newfe;
    ft->size++;
    return 0;
}

fileEntry *fileTableGet(fileTable *ft, int fd) {
    if (fd == -1) {
        return NULL;
    }

    unsigned int hash_idx = (unsigned int)(fd & ft->capacity);
    fileEntry *fe = ft->entries[hash_idx]; 
    while (fe) {
        if (fe->fd == fd) {
            return fe;
        }
        fe = fe->next;
    }
    return NULL;
}

fileEntry *fileTableDelete(fileTable *ft, int fd) {
    fileEntry *prev, *next, *fe;
    unsigned int hash_idx = (unsigned int)(fd & ft->capacity);

    fe = ft->entries[hash_idx];
    prev = NULL;
    while (fe) {
        next = fe->next;
        if (fe->fd) {
            if (prev) {
                prev->next = next;
                fe->next = NULL;
                return fe;
            } else {
                ft->entries[hash_idx] = next;
                fe->next = NULL;
                return fe;
            }
        }
        prev = fe;
        fe = fe->next;
    } 
    return NULL;
}

int main(void) {
    fileTable *ft = fileTableNew();
    int fd = open("./ex.txt", O_RDONLY, 0644);
    fileTableAdd(ft,fd,"./ex.txt",8);
    fileEntry *fe = fileTableGet(ft,fd);

    printf("%s %d\n", fe->name, fe->name_len);
}
