# EXBIN build — C11 core + C++17 front end.  `make`, `make test`, `make install`.
CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CXXFLAGS?= -O2 -g -Wall -Wextra -std=c++17
INC     := -Iinclude -Isrc
PREFIX  ?= /usr/local
BUILD   := build

C_SRCS   := object chunk scanner compiler vm serialize regex natives toolkit api
C_OBJS   := $(addprefix $(BUILD)/,$(addsuffix .o,$(C_SRCS)))
LIB      := $(BUILD)/libexbin.a
BIN      := $(BUILD)/exbin

all: $(BIN) $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/*.h include/exbin.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(BUILD)/main.o: src/main.cpp include/exbin.hpp include/exbin.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(LIB): $(C_OBJS)
	ar rcs $@ $^

$(BIN): $(BUILD)/main.o $(LIB)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

# Sanitizer build used by `make test-asan`.
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        CXXFLAGS="-O1 -g -std=c++17 -fsanitize=address,undefined" all
	$(CXX) -fsanitize=address,undefined $(BUILD)/main.o $(LIB) -o $(BUILD)/exbin -lm

examples: $(LIB)
	$(CC) $(CFLAGS) $(INC) examples/embed.c $(LIB) -o $(BUILD)/embed_c -lm
	$(CXX) $(CXXFLAGS) $(INC) examples/embed.cpp $(LIB) -o $(BUILD)/embed_cpp -lm

test: $(BIN) examples
	@sh tests/run.sh $(BIN)
	@$(BUILD)/embed_c > /dev/null && echo "PASS  examples/embed.c" || { echo "FAIL  examples/embed.c"; exit 1; }
	@$(BUILD)/embed_cpp > /dev/null && echo "PASS  examples/embed.cpp" || { echo "FAIL  examples/embed.cpp"; exit 1; }

test-asan: asan
	@EXBIN_GC_STRESS=1 sh tests/run.sh $(BIN)

install: $(BIN) $(LIB)
	install -d $(PREFIX)/bin $(PREFIX)/lib $(PREFIX)/include $(PREFIX)/share/exbin/lib
	install -m 755 $(BIN) $(PREFIX)/bin/exbin
	install -m 644 $(LIB) $(PREFIX)/lib/libexbin.a
	install -m 644 include/exbin.h include/exbin.hpp $(PREFIX)/include/
	install -m 644 lib/*.exb $(PREFIX)/share/exbin/lib/

clean:
	rm -rf $(BUILD)

.PHONY: all examples test test-asan asan install clean
