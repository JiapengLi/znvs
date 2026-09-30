CC ?= cc
AR ?= ar
PYTHON ?= python3
BUILD ?= _build
PROFILE ?= full
PROFILES := full boot readonly
PROFILE_full := 0
PROFILE_boot := 1
PROFILE_readonly := 2
ifeq ($(filter $(PROFILE),$(PROFILES)),)
$(error PROFILE must be full, boot or readonly)
endif
OUT := $(BUILD)/$(PROFILE)
CPPFLAGS += -Iznvs
CFLAGS ?= -Os
WARN := -std=c99 -Wall -Wextra -Werror -Wpedantic

.PHONY: all test demo matrix reference sanitize measure clean
all: $(OUT)/libznvs.a
ifeq ($(PROFILE),full)
all: $(OUT)/demo
endif
$(OUT):
	mkdir -p "$@"
$(OUT)/znvs.o: znvs/znvs.c znvs/znvs.h | $(OUT)
	$(CC) $(CPPFLAGS) -DZNVS_PROFILE=$(PROFILE_$(PROFILE)) $(CFLAGS) $(WARN) -c $< -o $@
$(OUT)/libznvs.a: $(OUT)/znvs.o
	$(AR) rcs $@ $^
$(OUT)/demo: examples/basic.c $(OUT)/libznvs.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $< $(OUT)/libznvs.a $(LDFLAGS) $(LDLIBS) -o $@
$(OUT)/test: tests/test_znvs.c tests/test_v2_cases.h $(OUT)/libznvs.a
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
	rm -rf "$(OUT)"
