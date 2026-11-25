# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble and link one generated lk source tree (SRC) plus our own sources (EXTRA).
#   make -f build.mk SRC=out/gen OUT=out/build STOCK_LAYOUT=0 EXTRA="src/a.c src/b.S"
SRC ?= out/gen
OUT ?= out/build
EXTRA ?=
XCFLAGS ?=
CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
AS := $(CROSS)as
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
# STOCK_LAYOUT=1 reproduces the stock bytes; 0 lets code and data grow.
STOCK_LAYOUT ?= 1
ASFLAGS := -march=armv8.5-a+crypto+sha3+sm4 -I $(SRC)/include --defsym LK_STOCK_LAYOUT=$(STOCK_LAYOUT)
CFLAGS := -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only \
	-mstrict-align -fno-stack-protector -fno-asynchronous-unwind-tables -fno-common \
	-ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns -fno-zero-initialized-in-bss \
	-Wall -Wextra -Werror -Wstack-usage=1024 -Isrc $(XCFLAGS)

GEN_SRCS := $(sort $(wildcard $(SRC)/text/*.S)) $(SRC)/rodata.S $(SRC)/data.S $(SRC)/bss.S
GEN_OBJS := $(patsubst $(SRC)/%.S,$(OUT)/obj/gen/%.o,$(GEN_SRCS))
X_OBJS := $(patsubst %,$(OUT)/obj/x/%.o,$(EXTRA))

all: $(OUT)/lk.bin

$(OUT)/obj/gen/%.o: $(SRC)/%.S $(SRC)/include/lk.inc
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) -o $@ $<

$(OUT)/obj/x/%.c.o: %.c $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/obj/x/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The stock start code clears only the stock bss, so our objects must not have
# any: zero-initialised variables go to .data (-fno-zero-initialized-in-bss).
$(OUT)/lk.elf: $(GEN_OBJS) $(X_OBJS) $(SRC)/lk.ld
	@for o in $(X_OBJS); do \
		if $(CROSS)size -A $$o | awk '$$1 ~ /^(\.bss|COMMON)/ && $$2 > 0 { f = 1 } END { exit !f }'; then \
			echo "$$o has .bss (give the variable an initialiser)"; exit 1; fi; \
	done
	@$(LD) -nostdlib -static --no-dynamic-linker --build-id=none -z noexecstack -z norelro \
		--no-warn-rwx-segments -T $(SRC)/lk.ld -Map $(OUT)/lk.map -o $@ $(GEN_OBJS) $(X_OBJS)

$(OUT)/lk.bin: $(OUT)/lk.elf
	@$(OBJCOPY) -O binary $< $@

.PHONY: all
