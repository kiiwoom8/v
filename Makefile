CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS = -mwindows -lgdi32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lwinmm
SRCS = main.c config.c input.c hint.c tray.c
OBJS = $(SRCS:.c=.o)
TARGET = vim.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	-@del /Q $(TARGET) 2>nul || true
	-@del /Q *.o 2>nul || true

.PHONY: all clean
