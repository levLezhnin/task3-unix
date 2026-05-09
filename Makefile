CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2
TARGET = myinit
SRC = myinit.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
