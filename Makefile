CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS = -mwindows -lgdi32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lwinmm -lole32 -loleaut32 -luuid
SRCS = main.c config.c input.c hint.c tray.c uia.c
OBJS = $(SRCS:.c=.o)
TARGET = vim.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Console tool that lists what the element scan finds on the monitor under the cursor
probe: tools/uia_probe.exe

tools/uia_probe.exe: tools/uia_probe.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/uia_probe.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid

clean:
	-@del /Q $(TARGET) 2>nul || true
	-@del /Q *.o 2>nul || true
	-@del /Q tools\uia_probe.exe 2>nul || true

.PHONY: all clean probe
