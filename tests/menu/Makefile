# Menu navigation lane: src/game/menu_nav.cpp (a flick moves the cursor to
# the nearest cell that way, round to the far side, the free half with an
# item) and the controls around it (src/game/controls.cpp: flicks and slow
# tilts in an open panel, B as Escape in the game menu and the gold dialog,
# the other buttons as before), against made-up windows, under qemu's ARM11
# MPCore model. Run through `scripts/docker.sh test menu`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/menu
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-psabi -march=armv6k -marm -I$(ROOT)/src
SRCS    := menu_test.cpp $(ROOT)/src/game/menu_nav.cpp $(ROOT)/src/game/auto_target.cpp $(ROOT)/src/game/controls.cpp

.PHONY: run clean
run: $(OUT)/menu-tests
	$(QEMU) $(OUT)/menu-tests

$(OUT)/menu-tests: $(SRCS) $(ROOT)/src/game/menu_nav.h $(ROOT)/src/game/auto_target.h $(ROOT)/src/game/controls.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) -static

clean:
	rm -rf $(OUT)
