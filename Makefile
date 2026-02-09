CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lgdi32
SRCS = m.c
TARGET = mouse_control_windows.exe

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	-@del /Q $(TARGET) 2>nul || true
	-@del /Q *.o 2>nul || true

.PHONY: all clean
