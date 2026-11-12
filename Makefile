CC ?= gcc
DEBUG ?= 1
BUILD_DIR := build
LIB_DIR := $(BUILD_DIR)/lib
BIN_DIR := $(BUILD_DIR)/bin

CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DDEBUG_BUILD=$(DEBUG)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -fstack-protector-strong
LDFLAGS +=

COMMON_LIB := $(LIB_DIR)/libcommon.so
SERVER := $(BIN_DIR)/server
CLIENT := $(BIN_DIR)/client

.PHONY: all clean test test-robustness compare-robustness help
all: $(SERVER) $(CLIENT)

$(LIB_DIR) $(BIN_DIR):
	mkdir -p $@

$(COMMON_LIB): src/common.c include/common.h | $(LIB_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -Wl,-soname,libcommon.so -o $@ src/common.c

$(SERVER): src/server.c include/common.h $(COMMON_LIB) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/server.c -L$(LIB_DIR) -lcommon \
		-Wl,-rpath,'$$ORIGIN/../lib' $(LDFLAGS)

$(CLIENT): src/client.c include/common.h $(COMMON_LIB) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/client.c -L$(LIB_DIR) -lcommon \
		-Wl,-rpath,'$$ORIGIN/../lib' $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

test: all
	bash tests/concurrency_test.sh

# Run only the protected implementation's abnormal-input tests.
test-robustness: all
	bash tests/run_robustness_tests.sh

# Run paired baseline (protections disabled) vs robust comparisons.
compare-robustness: all
	bash tests/run_robustness_comparison.sh

help:
	@echo 'make [DEBUG=0|1]'
	@echo 'make test'
	@echo 'make test-robustness'
	@echo 'make compare-robustness'
	@echo 'make clean'
