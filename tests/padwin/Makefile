# PAD window lane: src/game/pad_story.cpp (names, a talk revealed at the
# game's pace, kept, every later story appended, cleared), the PAD tab in
# src/game/second_screen.cpp, the art's decoders (pad_art.cpp, mpq.cpp) and
# the SCREEN switches' defaults, built twice: as the repository builds it
# and with D2_DEFAULT_NO_PANEL_AUTO=1 (a personal build). Under qemu's ARM11
# MPCore model. Run through `scripts/docker.sh test padwin`; with
# PADWIN_GAME=<a 1.14d folder> the art is also built from those MPQs.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/padwin
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-psabi -march=armv6k -marm -I$(ROOT)/src
SRCS    := padwin_test.cpp $(addprefix $(ROOT)/src/game/,second_screen.cpp pad_story.cpp pad_art.cpp mpq.cpp \
           pklib_explode.cpp osk.cpp)
HDRS    := $(wildcard $(ROOT)/src/game/*.h)

.PHONY: run clean
run: $(OUT)/padwin-tests $(OUT)/padwin-tests-personal
	$(QEMU) $(OUT)/padwin-tests
	$(QEMU) $(OUT)/padwin-tests-personal

$(OUT)/padwin-tests: $(SRCS) $(HDRS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) -static

$(OUT)/padwin-tests-personal: $(SRCS) $(HDRS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -DD2_DEFAULT_NO_PANEL_AUTO=1 -o $@ $(SRCS) -static

clean:
	rm -rf $(OUT)
