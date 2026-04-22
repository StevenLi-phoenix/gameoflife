CC      ?= clang
CSTD    ?= -std=c11
OPT     ?= -O3 -march=native -flto
WARN    ?= -Wall -Wextra -Wno-unused-parameter
CFLAGS  += $(CSTD) $(OPT) $(WARN) -pthread
LDFLAGS += -pthread

# raylib via pkg-config, fallback to homebrew.
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS   := $(shell pkg-config --libs   raylib 2>/dev/null)
ifeq ($(strip $(RAYLIB_LIBS)),)
  RAYLIB_PREFIX := $(shell brew --prefix raylib 2>/dev/null)
  ifneq ($(strip $(RAYLIB_PREFIX)),)
    RAYLIB_CFLAGS := -I$(RAYLIB_PREFIX)/include
    RAYLIB_LIBS   := -L$(RAYLIB_PREFIX)/lib -lraylib
  endif
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  RAYLIB_LIBS += -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL
endif
ifeq ($(UNAME_S),Linux)
  RAYLIB_LIBS += -lm -lGL -lX11 -lpthread -ldl -lrt
endif

CFLAGS  += $(RAYLIB_CFLAGS)
LDLIBS  := $(RAYLIB_LIBS) -lm

SRC := src/main.c src/life.c src/renderer.c
OBJ := $(SRC:.c=.o)
BIN := gameoflife

.PHONY: all clean run debug
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: OPT := -O0 -g -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJ) $(BIN)
