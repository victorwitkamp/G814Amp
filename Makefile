# G814Amp.kext: plain clang build against Kernel.framework (no Xcode project).
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
KF       := $(SDK)/System/Library/Frameworks/Kernel.framework
OUT      ?= build
KEXT     := $(OUT)/G814Amp.kext
SRC      := G814Amp/G814Amp.cpp
CSRC     := G814Amp/G814Amp_info.c
CFLAGS   := -arch x86_64 -mkernel -nostdinc -fno-builtin -fno-common -mmacosx-version-min=15.0 -isysroot $(SDK) -I$(KF)/Headers -I$(KF)/PrivateHeaders -D__KERNEL__ -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRIVATE -O2
CXX      := clang++
CXXFLAGS := -arch x86_64 -mkernel -nostdinc -fno-builtin -fno-exceptions -fno-rtti -fno-common -fapple-kext \
            -mmacosx-version-min=15.0 -isysroot $(SDK) -I$(KF)/Headers -I$(KF)/PrivateHeaders \
            -D__KERNEL__ -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRIVATE \
            -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-inconsistent-missing-override
LDFLAGS  := -arch x86_64 -isysroot $(SDK) -Xlinker -kext -nostdlib -lkmodc++ -lkmod -lcc_kext -mmacosx-version-min=15.0

all: $(KEXT)

$(OUT)/G814Amp.o: $(SRC) G814Amp/G814Amp.hpp G814Amp/cs35l41_regs.h G814Amp/cs35l41_otp_maps.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $(SRC) -o $@

$(OUT)/G814Amp_info.o: $(CSRC)
	mkdir -p $(OUT)
	clang $(CFLAGS) -c $(CSRC) -o $@

$(KEXT): $(OUT)/G814Amp.o $(OUT)/G814Amp_info.o G814Amp/Info.plist
	rm -rf $(KEXT); mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/G814Amp.o $(OUT)/G814Amp_info.o -o $(KEXT)/Contents/MacOS/G814Amp
	cp G814Amp/Info.plist $(KEXT)/Contents/Info.plist
	chown -R root:wheel $(KEXT) 2>/dev/null || true; chmod -R 755 $(KEXT)
	shasum -a 256 $(KEXT)/Contents/MacOS/G814Amp

clean:
	rm -rf $(OUT)
.PHONY: all clean
