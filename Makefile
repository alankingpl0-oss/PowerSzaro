# Ustawienia kompilatora i flag optymalizacyjnych
CC = gcc
CFLAGS = -O3 -march=native -mtune=native -funroll-loops -ffast-math -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=199309L

TARGET = psz
SRC = main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)