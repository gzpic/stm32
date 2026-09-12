CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
CPPFLAGS += -Icommon
COMMON = common/protocol.c common/service.c common/commands.c

.PHONY: all test clean
all: build/i2c_request
build:
	mkdir -p build
build/i2c_request: jetson/main.c jetson/parse_number.h common/protocol.c common/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) jetson/main.c common/protocol.c -o $@
build/spi_loopback: jetson/spi_loopback.c | build
	$(CC) $(CFLAGS) jetson/spi_loopback.c -o $@
build/spi_basic_test: jetson/spi_basic_test.c | build
	$(CC) $(CFLAGS) jetson/spi_basic_test.c -o $@
build/test_protocol: tests/test_protocol.c jetson/parse_number.h $(COMMON) common/protocol.h common/service.h common/commands.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_protocol.c $(COMMON) -o $@
test: build/test_protocol
	./build/test_protocol
clean:
	rm -f build/i2c_request build/spi_request build/spi_loopback build/spi_basic_test build/test_protocol build/test_protocol_sanitized
