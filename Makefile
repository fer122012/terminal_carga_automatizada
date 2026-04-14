CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LDFLAGS = -lpthread

SRC     = main.c \
          src/terminal.c \
          src/camion.c \
          src/planificador.c \
          src/log.c \
          src/utils.c

TARGET  = terminal

.PHONY: all clean run-fifo run-rr

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET) logs/*.log

run-fifo: all
	./$(TARGET) fifo 6

run-rr: all
	./$(TARGET) rr 6
