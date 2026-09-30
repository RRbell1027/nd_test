CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += nd_driver.o

all: libnd.so test_nd nd_driver.ko

libnd.so: libnd.cpp nd.h nd_ioctl.h
	$(CXX) $(CXXFLAGS) -fPIC -shared -o $@ libnd.cpp

test_nd: test_nd.cpp nd.h libnd.so
	$(CXX) $(CXXFLAGS) -o $@ test_nd.cpp \
		-L. -lnd -Wl,-rpath,'$$ORIGIN'

nd_driver.ko: nd_driver.c nd_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f libnd.so test_nd