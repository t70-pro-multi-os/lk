# SPDX-License-Identifier: GPL-3.0-or-later
# Build a T70 Pro lk.img from your own stock lk.img (Teclast G3D1 V1.08).
#
#   make STOCK_LK_IMG=/path/to/lk.img          -> out/lk.img (stock lk + our code)
#   make check STOCK_LK_IMG=/path/to/lk.img    -> the generated source rebuilds the stock lk
#   make relocheck STOCK_LK_IMG=...            -> every stock reference in out/lk.img is intact
#
# No MediaTek code is stored here: tools/lk_disasm.py turns the stock payload
# into source under out/ at build time.
STOCK_LK_IMG ?= $(error set STOCK_LK_IMG to the stock lk.img (sha256 18f56d90...0846))
OUT ?= out
PYTHON ?= python3
EXTRA := $(sort $(wildcard src/*.c src/*.S))

all: $(OUT)/lk.img

$(OUT)/stock/parts.json: $(STOCK_LK_IMG) tools/lk_img.py
	$(PYTHON) tools/lk_img.py split $< $(OUT)/stock

$(OUT)/gen/lk.ld: $(OUT)/stock/parts.json tools/lk_disasm.py names.txt hooks.txt
	$(PYTHON) tools/lk_disasm.py $(OUT)/stock/00-lk.bin $(OUT)/gen --names names.txt --hooks hooks.txt >/dev/null

$(OUT)/lk.bin: $(OUT)/gen/lk.ld $(EXTRA) $(wildcard src/*.h) build.mk
	$(MAKE) -f build.mk SRC=$(OUT)/gen OUT=$(OUT)/build STOCK_LAYOUT=0 EXTRA="$(EXTRA)"
	cp $(OUT)/build/lk.bin $@

$(OUT)/lk.img: $(OUT)/lk.bin
	$(PYTHON) tools/lk_img.py repack $(STOCK_LK_IMG) $@ lk=$<

check: $(OUT)/stock/parts.json
	$(PYTHON) tools/lk_disasm.py $(OUT)/stock/00-lk.bin $(OUT)/gen-stock >/dev/null
	$(MAKE) -f build.mk SRC=$(OUT)/gen-stock OUT=$(OUT)/build-stock STOCK_LAYOUT=1
	$(PYTHON) tools/lk_compare.py $(OUT)/stock/00-lk.bin $(OUT)/build-stock/lk.bin

relocheck: $(OUT)/lk.bin
	$(PYTHON) tools/lk_relocheck.py $(OUT)/stock/00-lk.bin $(OUT)/gen $(OUT)/build/lk.elf \
		--names names.txt --hooks hooks.txt

# Menu logic on the build host, with the stock lk functions simulated.
HOSTCC ?= cc
test:
	@mkdir -p $(OUT)
	$(HOSTCC) -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DT70_HOST_TEST -Isrc \
		-o $(OUT)/host_menu_test tests/host_menu_test.c src/orange_wait.c src/menu.c src/config.c src/keys.c
	$(OUT)/host_menu_test

clean:
	rm -rf $(OUT)

.PHONY: all check relocheck test clean
