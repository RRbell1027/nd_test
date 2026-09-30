CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17

KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD := $(CURDIR)/build

all: $(BUILD)/libnd.so $(BUILD)/test_nd $(BUILD)/nd_driver.ko

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

$(BUILD)/nd_driver.ko: nd_driver.c nd_ioctl.h | $(BUILD)
	cp nd_driver.c nd_ioctl.h $(BUILD)/
	printf 'obj-m += nd_driver.o\n' > $(BUILD)/Makefile
	$(MAKE) -C $(KDIR) M=$(BUILD) modules

clean:
	rm -rf $(BUILD)