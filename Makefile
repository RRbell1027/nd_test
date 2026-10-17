CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17
CPPFLAGS += -I$(CURDIR)/include

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


.PHONY: all userspace driver clean print-config check sdk-check

all: userspace driver

userspace: $(BUILD)/libnd.so $(BUILD)/test_nd

driver: $(BUILD)/nd_driver.ko

check: userspace
	python3 tests/host/smoke.py $(BUILD)

sdk-check:
	python3 tests/luckfox-sdk/validate.py


$(BUILD):
	mkdir -p $(BUILD)


$(BUILD)/libnd.so: src/libnd.cpp include/nd.h include/nd_driver.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -shared \
		-o $@ src/libnd.cpp


$(BUILD)/test_nd: examples/test_nd.cpp include/nd.h $(BUILD)/libnd.so | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		-o $@ examples/test_nd.cpp \
		-L$(BUILD) -lnd \
		-Wl,-rpath,'$$ORIGIN'


$(BUILD)/nd_driver.ko: drivers/host/nd_driver.c include/nd_driver.h include/nd_protocol.h | $(BUILD)
	cp drivers/host/nd_driver.c include/nd_driver.h include/nd_protocol.h $(BUILD)/
	printf 'obj-m += nd_driver.o\n' > $(BUILD)/Makefile
	$(MAKE) -C $(KDIR) M=$(BUILD) modules


clean:
	rm -rf $(BUILD)


print-config:
	@echo "IS_WSL = $(IS_WSL)"
	@echo "KDIR   = $(KDIR)"
	@echo "BUILD  = $(BUILD)"
