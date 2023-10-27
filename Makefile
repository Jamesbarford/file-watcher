TARGET := watchme.out
CC     := gcc
OUTDIR := .
CFLAGS = -O0 -DDEBUG

$(OUTDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

all: $(TARGET)

OBJS = watcher.o fw.o

$(TARGET): $(OBJS)
	$(CC) -o $(TARGET) $(OBJS)

clean:
	rm -rf ./*.o
	rm -rf $(TARGET)

format-code:
	clang-format *.c -i

format-headers:
	clang-format *.h -i

format: format-code format-headers

watcher.o: watcher.c fw.h
fw.o: fw.c fw.h
