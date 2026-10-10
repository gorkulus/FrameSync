RACK_DIR ?= ../Rack-SDK
SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += LICENSE.md LICENSE-GPLv3.txt README.md VALIDATION.md examples
EXTRA_CXXFLAGS += -std=c++17 -fno-fast-math
EXTRA_FLAGS += -fno-unsafe-math-optimizations -fno-finite-math-only
LDFLAGS += -pthread
include $(RACK_DIR)/plugin.mk

.PHONY: test test-controls
test:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread -Isrc tests/core_test.cpp src/CaptureSession.cpp -o build/core_test
	./build/core_test

test-controls:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -Isrc -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include tests/control_test.cpp src/CaptureSession.cpp -L$(RACK_DIR) -Wl,-rpath,$(abspath $(RACK_DIR)) -lRack -o build/control_test
	./build/control_test
	python3 tests/check_controls.py
