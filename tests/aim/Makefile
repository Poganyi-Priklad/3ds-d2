# Auto-target lane: src/game/auto_target.cpp (the cone, the rules, the
# target's stickiness and timers), src/game/aim_settings.h (settings.txt's
# aim keys) and the controls with the auto-target (src/game/controls.cpp),
# against made-up units, under qemu's ARM11 MPCore model. Run through
# `scripts/docker.sh test aim`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/aim
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-psabi -march=armv6k -marm -I$(ROOT)/src
SRCS    := aim_test.cpp $(ROOT)/src/game/auto_target.cpp $(ROOT)/src/game/controls.cpp $(ROOT)/src/game/menu_nav.cpp

.PHONY: run clean
run: $(OUT)/aim-tests
	$(QEMU) $(OUT)/aim-tests

$(OUT)/aim-tests: $(SRCS) $(ROOT)/src/game/auto_target.h $(ROOT)/src/game/aim_settings.h \
                  $(ROOT)/src/game/settings_file.h $(ROOT)/src/game/controls.h $(ROOT)/src/game/menu_nav.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) -static

clean:
	rm -rf $(OUT)
