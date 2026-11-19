# Pad lane: src/game/pad_history.h (presses shorter than a frame, issue
# #118) under qemu's ARM11 MPCore model. Run through `scripts/docker.sh test pad`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/pad
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-psabi -march=armv6k -marm -I$(ROOT)/src

.PHONY: run clean
run: $(OUT)/pad-tests
	$(QEMU) $(OUT)/pad-tests

$(OUT)/pad-tests: pad_test.cpp $(ROOT)/src/game/pad_history.h $(ROOT)/src/game/controls.h $(ROOT)/src/game/controls.cpp \
                  $(ROOT)/src/game/auto_target.h $(ROOT)/src/game/auto_target.cpp
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ pad_test.cpp $(ROOT)/src/game/controls.cpp $(ROOT)/src/game/auto_target.cpp -static

clean:
	rm -rf $(OUT)
