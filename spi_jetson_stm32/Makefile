CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
CPPFLAGS += -Icommon
COMMON = common/protocol.c common/service.c common/commands.c

.PHONY: all test clean
all: build/spi_request
build:
	mkdir -p build
build/spi_request: jetson/main.c jetson/parse_number.h common/protocol.c common/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) jetson/main.c common/protocol.c -o $@
build/test_protocol: tests/test_protocol.c jetson/parse_number.h $(COMMON) common/protocol.h common/service.h common/commands.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_protocol.c $(COMMON) -o $@
test: build/test_protocol
	./build/test_protocol
clean:
	rm -f build/spi_request build/test_protocol
