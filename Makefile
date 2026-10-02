CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17

BUILD := $(CURDIR)/build

# Detect WSL
IS_WSL := $(shell grep -qi microsoft /proc/version 2>/dev/null && echo 1 || echo 0)

ifeq ($(IS_WSL),1)
    # WSL does not provide the usual /lib/modules/.../build tree.
    # Use the prepared Microsoft WSL kernel source/build tree.
    KDIR ?= /sdk/WSL2-Linux-Kernel
else
    # Normal Linux distribution
    KDIR ?= /lib/modules/$(shell uname -r)/build
endif


all: $(BUILD)/libnd.so \
     $(BUILD)/test_nd \
     $(BUILD)/nd_driver.ko


$(BUILD):
	mkdir -p $(BUILD)


$(BUILD)/libnd.so: libnd.cpp nd.h nd_ioctl.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC -shared \
		-o $@ libnd.cpp


$(BUILD)/test_nd: test_nd.cpp nd.h $(BUILD)/libnd.so | $(BUILD)
	$(CXX) $(CXXFLAGS) \
		-o $@ test_nd.cpp \
		-L$(BUILD) -lnd \
		-Wl,-rpath,'$$ORIGIN'


$(BUILD)/nd_driver.ko: nd_driver.c nd_ioctl.h nd_protocol.h | $(BUILD)
	cp nd_driver.c nd_ioctl.h nd_protocol.h $(BUILD)/
	printf 'obj-m += nd_driver.o\n' > $(BUILD)/Makefile
	$(MAKE) -C $(KDIR) M=$(BUILD) modules


clean:
	rm -rf $(BUILD)


print-config:
	@echo "IS_WSL = $(IS_WSL)"
	@echo "KDIR   = $(KDIR)"
	@echo "BUILD  = $(BUILD)"