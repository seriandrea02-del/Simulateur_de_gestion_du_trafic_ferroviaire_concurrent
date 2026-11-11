CC      ?= gcc
OPT     ?= -O2 -g
SAN     ?=
CFLAGS  += $(OPT) $(SAN) -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
           -Wall -Wextra -Wpedantic -Iinclude -pthread
LDFLAGS += -pthread $(SAN)

SRC  = src/main.c src/train.c src/network.c src/control.c src/util.c
OBJ  = $(SRC:src/%.c=build/%.o)
BIN  = simu_ferroviaire

.PHONY: all clean run run-prev test tsan

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: src/%.c include/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

run: $(BIN)
	./$(BIN) -d 40 -m detection

run-prev: $(BIN)
	./$(BIN) -d 40 -m prevention

# Tests : les deux modes doivent se terminer sans violation de sécurité
test: $(BIN)
	./tests/run_tests.sh ./$(BIN)

# Analyse dynamique des courses de données (ThreadSanitizer)
tsan:
	$(MAKE) clean
	$(MAKE) OPT="-O1 -g" SAN="-fsanitize=thread"

clean:
	rm -rf build $(BIN)
