# Settings lane: src/game/settings_file.h (settings.txt rewritten without
# losing the lines the port doesn't own; its writes coalesced) under qemu's
# ARM11 MPCore model. Run through `scripts/docker.sh test settings`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/settings
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-psabi -march=armv6k -marm -I$(ROOT)/src

.PHONY: run clean
run: $(OUT)/settings-tests
	$(QEMU) $(OUT)/settings-tests

$(OUT)/settings-tests: settings_test.cpp $(ROOT)/src/game/settings_file.h $(ROOT)/src/game/aim_settings.h $(ROOT)/src/game/auto_target.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ settings_test.cpp -static

clean:
	rm -rf $(OUT)
