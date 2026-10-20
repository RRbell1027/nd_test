CXX ?= g++
OBJCOPY ?= objcopy
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


$(BUILD)/libnd.so: runtime src/libnd.cpp include/nd.h include/nd_driver.h include/nd_protocol.h include/nd_rpc_wire.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -shared \
		-o $@ src/libnd.cpp -L$(BUILD)/runtime -lnd_runtime -pthread -Wl,-rpath,'$$ORIGIN/runtime'


$(BUILD)/test_nd: examples/test_nd.cpp include/nd.h $(BUILD)/libnd.so | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		-o $@ examples/test_nd.cpp \
		-L$(BUILD) -lnd \
		-Wl,-rpath,'$$ORIGIN'


$(BUILD)/nd_driver.ko: drivers/host/nd_driver.c include/nd_driver.h include/nd_protocol.h include/nd_rpc_wire.h | $(BUILD)
	cp drivers/host/nd_driver.c include/nd_driver.h include/nd_protocol.h include/nd_rpc_wire.h $(BUILD)/
	printf 'obj-m += nd_driver.o\n' > $(BUILD)/Makefile
	$(MAKE) -C $(KDIR) M=$(BUILD) modules
ifeq ($(IS_WSL),1)
	$(OBJCOPY) --remove-section=.BTF --remove-section=.BTF.ext $@
endif


clean:
	rm -rf $(BUILD)


print-config:
	@echo "IS_WSL = $(IS_WSL)"
	@echo "KDIR   = $(KDIR)"
	@echo "BUILD  = $(BUILD)"

# Runtime core is independent from the LLVM build; libffi is needed by executor.
.PHONY: runtime runtime-check
runtime:
	cmake -S runtime -B $(BUILD)/runtime $(RUNTIME_CMAKE_ARGS)
	cmake --build $(BUILD)/runtime -j2

runtime-check: runtime
	$(CC) -shared -fPIC examples/runtime_kernel.c -o $(BUILD)/runtime/kernel-a.so
	sed 's/+ 7/+ 19/' examples/runtime_kernel.c > $(BUILD)/runtime/kernel-b.c
	$(CC) -shared -fPIC $(BUILD)/runtime/kernel-b.c -o $(BUILD)/runtime/kernel-b.so
	python3 tests/runtime/image_roundtrip.py --lib $(BUILD)/runtime/libnd_runtime.so --executor $(BUILD)/runtime/nd-executor $(BUILD)/runtime/kernel-a.so $(BUILD)/runtime/kernel-b.so

# Native Linux Guest LLVM/OpenMP/NDS bring-up.
LLVM_BUILD ?= $(BUILD)/llvm
OPENMP_BUILD ?= $(BUILD)/openmp
OFFLOAD_BUILD ?= $(BUILD)/offload-native
NDS_JOBS ?= 3
NDS_PLATFORM ?= platform/guest.local.ini
NDS_CLANG := $(abspath $(LLVM_BUILD))/bin/clang
NDS_CXX := $(abspath $(LLVM_BUILD))/bin/clang++
ND_RUNTIME_DEVICE_PATH ?= /dev/ttyS0

.PHONY: llvm-guest openmp-runtime offload-runtime nds-runtime nds-compile nds-check runtime-uart-bridge
llvm-guest:
	cmake -G Ninja -S llvm-project/llvm -B $(LLVM_BUILD) -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS_RELEASE='-O0 -DNDEBUG' -DCMAKE_CXX_FLAGS_RELEASE='-O0 -DNDEBUG' \
		-DCMAKE_EXE_LINKER_FLAGS='-Wl,--gc-sections' -DLLVM_ENABLE_PROJECTS=clang \
		-DLLVM_TARGETS_TO_BUILD='X86;ARM' -DLLVM_PARALLEL_LINK_JOBS=1 \
		-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
		-DLLVM_INCLUDE_BENCHMARKS=OFF -DCLANG_INCLUDE_TESTS=OFF
	cmake --build $(LLVM_BUILD) --target clang clang-offload-packager clang-linker-wrapper -j$(NDS_JOBS)
	test -e $(NDS_CXX) || ln -s clang $(NDS_CXX)

openmp-runtime:
	cmake -G Ninja -S llvm-project/openmp -B $(OPENMP_BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(OPENMP_BUILD) -j$(NDS_JOBS)

offload-runtime: llvm-guest runtime openmp-runtime
	cmake -G Ninja -S llvm-project/offload -B $(OFFLOAD_BUILD) -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=$(NDS_CLANG) -DCMAKE_CXX_COMPILER=$(NDS_CXX) \
		-DCMAKE_EXE_LINKER_FLAGS='-Wl,--gc-sections' \
		-DLLVM_DIR=$(abspath $(LLVM_BUILD))/lib/cmake/llvm \
		-DLIBOMPTARGET_PLUGINS_TO_BUILD=nds -DNDS_RUNTIME_INCLUDE_DIR=$(CURDIR)/include \
		-DNDS_RUNTIME_LIBRARY=$(abspath $(BUILD))/runtime/libnd_runtime.so \
		-DLIBOMP_STANDALONE=$(abspath $(OPENMP_BUILD))/runtime/src/libomp.so -DOFFLOAD_INCLUDE_TESTS=OFF
	cmake --build $(OFFLOAD_BUILD) --target omptarget -j$(NDS_JOBS)

nds-runtime: offload-runtime

nds-compile: nds-runtime
	$(NDS_CXX) -std=c++17 -fopenmp -fopenmp-targets=arm-nds-linux-gnueabihf \
		-nds-platform=$(NDS_PLATFORM) -fno-addrsig \
		-Xopenmp-target=arm-nds-linux-gnueabihf -mcpu=cortex-a7 \
		examples/omp_runtime_smoke.cpp -o $(BUILD)/omp_runtime_smoke \
		-L$(abspath $(OPENMP_BUILD))/runtime/src -L$(abspath $(OFFLOAD_BUILD)) \
		-Wl,-rpath,$(abspath $(OPENMP_BUILD))/runtime/src -Wl,-rpath,$(abspath $(OFFLOAD_BUILD))

nds-check: nds-compile
	OMP_TARGET_OFFLOAD=MANDATORY ND_RUNTIME_DEVICE_PATH=$(ND_RUNTIME_DEVICE_PATH) $(BUILD)/omp_runtime_smoke 7
	OMP_TARGET_OFFLOAD=MANDATORY ND_RUNTIME_DEVICE_PATH=$(ND_RUNTIME_DEVICE_PATH) $(BUILD)/omp_runtime_smoke 19

runtime-uart-bridge:
	python3 runtime/uart_bridge.py --uart $(or $(NDS_UART),/dev/ttyUSB0) --socket $(NDS_BRIDGE_SOCKET)

# Luckfox executor cross-build inside Guest; ARM libffi comes from board SDK.
NDS_DEVICE_CC ?= $(HOME)/nds-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
NDS_DEVICE_FFI_ROOT ?= $(HOME)/nds-sdk/arm-libffi
NDS_BRIDGE_SOCKET ?= /tmp/nds-runtime-bridge.sock
.PHONY: runtime-arm
runtime-arm:
	test -f $(NDS_DEVICE_FFI_ROOT)/include/ffi.h
	mkdir -p $(BUILD)/runtime/arm
	$(NDS_DEVICE_CC) -std=c11 -Os -s -mcpu=cortex-a7 -mfpu=vfpv4-d16 -mfloat-abi=hard \
		-I$(CURDIR)/include -I$(NDS_DEVICE_FFI_ROOT)/include runtime/executor.c \
		-L$(NDS_DEVICE_FFI_ROOT)/lib -Wl,-rpath,/usr/lib -lffi -ldl \
		-o $(BUILD)/runtime/arm/nd-executor

.PHONY: runtime-uart-start runtime-guest-port
runtime-uart-start:
	python3 tests/runtime/start_uart_executor.py --uart $(or $(NDS_UART),/dev/ttyUSB0) \
		--executor $(or $(NDS_EXECUTOR),$(BUILD)/runtime/arm/nd-executor)

runtime-guest-port:
	python3 tests/runtime/attach_guest_port.py --qmp $(or $(NDS_QMP),/tmp/near-data-qmp.sock) \
		--socket $(NDS_BRIDGE_SOCKET)

# USB runtime path: Guest ttyS0 -> QEMU /dev/nd0 -> ND USB -> /dev/nd_rpc.
ND_USB_TRANSPORT ?= /dev/nd0
.PHONY: runtime-usb-check runtime-usb-bundle nds-usb-check
runtime-usb-check: runtime
	python3 tests/hardware/usb_runtime_smoke.py --lib $(BUILD)/runtime/libnd_runtime.so --transport $(ND_USB_TRANSPORT)

runtime-usb-bundle: runtime-arm
	cp runtime/nd-executor-service.sh $(BUILD)/runtime/arm/nd-executor-service.sh

nds-usb-check: ND_RUNTIME_DEVICE_PATH=/dev/ttyS0
nds-usb-check: nds-check

# Public host API, statically linked to the same runtime used by the plugin.
.PHONY: device-api-check runtime-install
$(BUILD)/device_memory: examples/device_memory.c include/nd_device.h runtime
	$(CC) $(CPPFLAGS) -Wall -Wextra $< -L$(BUILD)/runtime -lnd_runtime -pthread -Wl,-rpath,'$$ORIGIN/runtime' -o $@
device-api-check: $(BUILD)/device_memory
	$(BUILD)/device_memory $(ND_USB_TRANSPORT)
runtime-install: runtime
	cmake --install $(BUILD)/runtime $(if $(PREFIX),--prefix "$(PREFIX)",)

# A file on the actual exported SD backing is required, not overlay-only data.
NDS_INPUT_FILE ?= /usr/share/common-licenses/GPL-3
.PHONY: nds-file-compile nds-file-check
nds-file-compile: nds-runtime
	$(NDS_CXX) -std=c++17 -Iinclude -fopenmp -fopenmp-targets=arm-nds-linux-gnueabihf \
		-nds-platform=$(NDS_PLATFORM) -fno-addrsig \
		-Xopenmp-target=arm-nds-linux-gnueabihf -mcpu=cortex-a7 \
		examples/omp_file_read.cpp -o $(BUILD)/omp_file_read \
		-L$(BUILD)/runtime -lnd -lnd_runtime \
		-L$(abspath $(OPENMP_BUILD))/runtime/src -L$(abspath $(OFFLOAD_BUILD)) \
		-Wl,-rpath,$(BUILD)/runtime -Wl,-rpath,$(abspath $(OPENMP_BUILD))/runtime/src \
		-Wl,-rpath,$(abspath $(OFFLOAD_BUILD))
nds-file-check: nds-file-compile
	OMP_TARGET_OFFLOAD=MANDATORY ND_RUNTIME_DEVICE_PATH=$(ND_RUNTIME_DEVICE_PATH) $(BUILD)/omp_file_read $(NDS_INPUT_FILE)

.PHONY: runtime-file-check
runtime-file-check: runtime
	python3 tests/runtime/file_device.py $(BUILD)/runtime
