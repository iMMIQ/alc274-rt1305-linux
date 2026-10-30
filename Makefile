# SPDX-License-Identifier: GPL-2.0-only
KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_SRC ?=
BUILD_DIR := $(CURDIR)/.build
HOSTCC ?= cc
HOSTCFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter

.PHONY: all prepare test clean

all: prepare
	$(MAKE) -C "$(KDIR)" M="$(BUILD_DIR)" modules

prepare:
	@test -n "$(KERNEL_SRC)" || { echo 'Set KERNEL_SRC to the matching Linux source tree.' >&2; exit 1; }
	@test -f "$(KERNEL_SRC)/sound/pci/hda/patch_realtek.c" || { echo 'Expected the Linux 6.12 sound/pci/hda source layout.' >&2; exit 1; }
	@test -f "$(KDIR)/Module.symvers" || { echo 'Matching kernel headers and Module.symvers are required.' >&2; exit 1; }
	@mkdir -p "$(BUILD_DIR)"
	cp "$(KERNEL_SRC)/sound/pci/hda/patch_realtek.c" "$(BUILD_DIR)/"
	cp "$(KERNEL_SRC)"/sound/pci/hda/*_helper.c "$(BUILD_DIR)/"
	cp "$(KERNEL_SRC)"/sound/pci/hda/*.h "$(BUILD_DIR)/"
	cp kernel/alc274_rt1305.c kernel/alc274_rt1305_eq.h "$(BUILD_DIR)/"
	cp Kbuild "$(BUILD_DIR)/Kbuild"
	patch --batch --forward -d "$(BUILD_DIR)" -p1 < kernel/realtek.patch

test:
	@mkdir -p "$(BUILD_DIR)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG tests/driver_test.c -o "$(BUILD_DIR)/driver_test"
	"$(BUILD_DIR)/driver_test"

clean:
	rm -rf -- "$(BUILD_DIR)"
