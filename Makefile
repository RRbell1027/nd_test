CXX ?= g++

CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17

all: libnd.so test_nd

libnd.so: libnd.cpp nd.h
        $(CXX) $(CXXFLAGS) -fPIC -shared -o $@ libnd.cpp

test_nd: test_nd.cpp nd.h libnd.so
        $(CXX) $(CXXFLAGS) -o $@ test_nd.cpp -L. -lnd -Wl,-rpath,'$$ORIGIN'

clean:
        rm -f libnd.so test_nd