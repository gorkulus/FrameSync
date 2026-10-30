RACK_DIR ?= ../Rack-SDK
SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += LICENSE.md LICENSE-GPLv3.txt README.md VALIDATION.md WINDOWS-SETUP.md examples docs/assets/framesync-area-selection.png
EXTRA_CXXFLAGS += -std=c++17 -fno-fast-math
EXTRA_FLAGS += -fno-unsafe-math-optimizations -fno-finite-math-only
LDFLAGS += -pthread
include $(RACK_DIR)/plugin.mk

ifdef ARCH_WIN
LDFLAGS += -lopengl32 -static-libgcc
TEST_SUFFIX := .exe
TEST_CXXFLAGS := -D_USE_MATH_DEFINES -march=nehalem
TEST_LDFLAGS := -static-libstdc++ -static-libgcc
RACK_RUNTIME_DIR ?= $(RACK_DIR)
TEST_RUNTIME := PATH="$(RACK_RUNTIME_DIR):$$PATH"
PYTHON ?= python
else
TEST_LDFLAGS := -Wl,-rpath,$(abspath $(RACK_DIR))
PYTHON ?= python3
endif

.PHONY: test test-controls test-races test-races-negative
ifdef ARCH_WIN
.PHONY: test-windows
test-windows:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread -Isrc tests/windows_test.cpp src/CaptureSession.cpp $(TEST_LDFLAGS) -o build/windows_test.exe
	./build/windows_test.exe
	$(PYTHON) tests/check_windows.py
endif
test:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread -Isrc tests/core_test.cpp src/CaptureSession.cpp $(TEST_LDFLAGS) -o build/core_test$(TEST_SUFFIX)
	./build/core_test$(TEST_SUFFIX)
	$(PYTHON) tests/check_outputs.py

test-controls:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread $(TEST_CXXFLAGS) -Isrc -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include tests/control_test.cpp src/CaptureSession.cpp -L$(RACK_DIR) -lRack $(TEST_LDFLAGS) -o build/control_test$(TEST_SUFFIX)
	$(TEST_RUNTIME) ./build/control_test$(TEST_SUFFIX)
	$(PYTHON) tests/check_controls.py

# Hooks are compiled only into this regression executable, never the plugin.
test-races:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread -DFRAMESYNC_TEST_HOOKS -Isrc tests/race_test.cpp src/CaptureSession.cpp $(TEST_LDFLAGS) -o build/race_test$(TEST_SUFFIX)
	./build/race_test$(TEST_SUFFIX)
	$(PYTHON) tests/check_races.py

# Mutation checks prove the scheduler actually exposes the original predicates.
test-races-negative:
	$(PYTHON) tests/reproduce_races.py --cxx "$(CXX)"

ifdef ARCH_LIN
.PHONY: test-posix
test-posix:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread -Isrc tests/posix_test.cpp src/CaptureSession.cpp -o build/posix_test
	./build/posix_test
	$(PYTHON) tests/check_posix.py
endif
