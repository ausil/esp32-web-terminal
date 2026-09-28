# Host unit tests for esp32-web-terminal — no ESP-IDF required.
#   make test     build + run every suite
#   make clean
#
# The suites compile the real sources from main/ against stubbed IDF headers
# (stubs/). The WS TX ring test compiles a slice extracted from
# main/web_server.c (extract_ring.py) so it can never drift from production.

CC       ?= gcc
CSTD     ?= -std=gnu11
WARN      = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
            -Wno-format   # uint32_t is `unsigned int` on glibc, `unsigned long` on IDF
OPT      ?= -O1 -g
CFLAGS   = $(CSTD) $(WARN) $(OPT) -Istubs -I$(ROOT)/main -Iunity -Icjson
LDFLAGS  = -lcrypto -lm

ROOT     := ../..
BUILD    := build

# Vendored third-party (see unity/LICENSE.txt, cjson/LICENSE)
UNITY_SRCS = unity/unity.c
CJSON_O  = $(BUILD)/cjson.o

# Stubs shared by every suite
STUB_OBJS = $(BUILD)/esp_log.o $(BUILD)/clock_random.o $(BUILD)/nvs.o \
            $(BUILD)/md_stub.o $(BUILD)/httpd_fake.o $(BUILD)/req_fake.o
UNITY_OBJS = $(BUILD)/unity.o

SUITES := test_config test_auth test_ota test_ring
BINS   := $(addprefix $(BUILD)/,$(SUITES))

.PHONY: all test clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

# Generated: the ring extracted from web_server.c. If web_server.c loses the
# markers this fails loudly instead of testing a stale copy.
$(BUILD)/ring_extracted.c: $(ROOT)/main/web_server.c extract_ring.py | $(BUILD)
	python3 extract_ring.py > $@.tmp && mv $@.tmp $@

$(BUILD)/%.o: $(ROOT)/main/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: stubs/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/unity.o: unity/unity.c | $(BUILD)
	$(CC) $(CSTD) -O1 -c $< -o $@

$(BUILD)/cjson.o: cjson/cJSON.c | $(BUILD)
	$(CC) $(CSTD) -O1 -c $< -o $@

$(BUILD)/ring_extracted.o: $(BUILD)/ring_extracted.c stubs/test_ring_shim.h | $(BUILD)
	$(CC) $(CFLAGS) -include sdkconfig.h -Dauth_validate_session=fake_auth_validate_session -c $< -o $@

$(BUILD)/test_ring.o: test_ring.c | $(BUILD)
	$(CC) $(CFLAGS) -include sdkconfig.h -c $< -o $@

# Production code includes <sdkconfig.h>-guarded headers; force-include ours.
$(BUILD)/auth.o: $(ROOT)/main/auth.c | $(BUILD)
	$(CC) $(CFLAGS) -include sdkconfig.h -c $< -o $@

$(BUILD)/config.o: $(ROOT)/main/config.c | $(BUILD)
	$(CC) $(CFLAGS) -include sdkconfig.h -c $< -o $@

$(BUILD)/ota_github.o: $(ROOT)/main/ota_github.c | $(BUILD)
	$(CC) $(CFLAGS) -include sdkconfig.h -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -include sdkconfig.h -c $< -o $@

$(BUILD)/test_config: $(BUILD)/test_config.o $(BUILD)/config.o $(STUB_OBJS) $(UNITY_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_auth: test_auth.c $(BUILD)/auth.o $(BUILD)/config.o $(BUILD)/cjson.o $(STUB_OBJS) $(UNITY_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_ota: test_ota.c $(BUILD)/ota_github.o $(BUILD)/cjson.o $(BUILD)/http_ota_stubs.o $(STUB_OBJS) $(UNITY_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_ring: $(BUILD)/test_ring.o $(BUILD)/ring_extracted.o $(STUB_OBJS) $(UNITY_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(BINS)
	@fail=0; \
	for b in $(BINS); do \
		echo "== $$b =="; \
		./$$b || fail=1; \
	done; \
	exit $$fail

clean:
	rm -rf $(BUILD)
