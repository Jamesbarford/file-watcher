TARGET := watchme.out
CC     := gcc
OUTDIR := .
CFLAGS = -O0 -DDEBUG -g

$(OUTDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

all: $(TARGET)

OBJS = $(OUTDIR)/main.o $(OUTDIR)/fw.o

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


$(OUTDIR)/main.o: main.c fw.h
$(OUTDIR)/fw.o: fw.c fw.h
