CC ?= cc
AR ?= ar
PYTHON ?= python3
BUILD ?= _build
CRC ?= 1
CACHE ?= 0
IO ?= 32
# Distinct object paths prevent stale ABI/config reuse when changing options.
OUT := $(BUILD)/crc$(CRC)-cache$(CACHE)-io$(IO)
CPPFLAGS += -Iznvs -DZNVS_DATA_CRC=$(CRC) -DZNVS_CACHE_SIZE=$(CACHE) -DZNVS_IO_SIZE=$(IO)
CFLAGS ?= -Os
WARN := -std=c99 -Wall -Wextra -Werror -Wpedantic

.PHONY: all test demo matrix reference sanitize measure clean
all: $(OUT)/libznvs.a $(OUT)/demo
$(OUT):
	mkdir -p "$@"
$(OUT)/znvs.o: znvs/znvs.c znvs/znvs.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c $< -o $@
$(OUT)/libznvs.a: $(OUT)/znvs.o
	$(AR) rcs $@ $^
$(OUT)/demo: examples/basic.c $(OUT)/libznvs.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $< $(OUT)/libznvs.a $(LDFLAGS) $(LDLIBS) -o $@
$(OUT)/test: tests/test_znvs.c $(OUT)/libznvs.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $< $(OUT)/libznvs.a $(LDFLAGS) $(LDLIBS) -o $@
test: $(OUT)/test
	$(OUT)/test > $(OUT)/test.log
	cat $(OUT)/test.log
demo: $(OUT)/demo
	$(OUT)/demo
matrix:
	$(PYTHON) tools/check.py --cc "$(CC)" --out "$(BUILD)/matrix"
reference:
	$(PYTHON) tools/check.py --cc "$(CC)" --out "$(BUILD)/reference" --reference-only
sanitize:
	$(PYTHON) tools/check.py --cc clang --out "$(BUILD)/sanitize" --sanitize-only
measure:
	$(PYTHON) tools/measure.py --out "$(BUILD)/measure"
clean:
	rm -rf "$(BUILD)"
