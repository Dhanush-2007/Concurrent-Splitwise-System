CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
# -lrt is Linux-only; macOS links these automatically
UNAME := $(shell uname)
ifeq ($(UNAME), Linux)
  LDFLAGS = -pthread -lrt
else
  LDFLAGS = -pthread
endif

SERVER_SRC = server.c
CLIENT_SRC = client.c
HEADERS    = common.h auth.h filelock.h ipc.h expense.h

.PHONY: all clean run_server run_client data

all: server client

server: $(SERVER_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o server $(SERVER_SRC) $(LDFLAGS)
	@echo ">>> server built"

client: $(CLIENT_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o client $(CLIENT_SRC) $(LDFLAGS)
	@echo ">>> client built"

data:
	mkdir -p data

run_server: server data
	./server

run_client: client
	./client

clean:
	rm -f server client
	rm -rf data
	rm -f /tmp/expense_notify.fifo
	-ipcrm -M 0x1234 2>/dev/null || true
	-ipcrm -Q 0x5678 2>/dev/null || true
	-sem_unlink /expense_notify 2>/dev/null || true
