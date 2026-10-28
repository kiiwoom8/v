CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS = -mwindows -lgdi32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lwinmm -lole32 -loleaut32 -luuid -lcomdlg32
SRCS = main.c config.c input.c hint.c tray.c uia.c snippet.c
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
	$(CC) $(CFLAGS) -I. -o $@ tools/uia_probe.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32

# Console tool that compares Chrome's DOM with what the scan finds (hint_check.mjs)
check: tools/hint_check.exe

tools/hint_check.exe: tools/hint_check.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/hint_check.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32

clean:
	-@del /Q $(TARGET) 2>nul || true
	-@del /Q *.o 2>nul || true
	-@del /Q tools\uia_probe.exe 2>nul || true
	-@del /Q tools\hint_check.exe 2>nul || true

.PHONY: all clean probe check
