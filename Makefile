SHELL := /bin/zsh

VCPKG_ROOT ?= /home/jvsx/Projetos/personal/vcpkg
BUILD_DIR ?= build-local
BUILD_TYPE ?= Release

CMAKE_TOOLCHAIN_FILE := $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
CMAKE_CONFIGURE := cmake -S . -B $(BUILD_DIR) -G Ninja \
	-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	-DCMAKE_TOOLCHAIN_FILE=$(CMAKE_TOOLCHAIN_FILE)

.PHONY: configure build api preprocess benchmark run-api run-preprocess run-benchmark clean

configure:
	$(CMAKE_CONFIGURE)

build: configure
	cmake --build $(BUILD_DIR)

api: configure
	cmake --build $(BUILD_DIR) --target fraud_api

preprocess: configure
	cmake --build $(BUILD_DIR) --target preprocess_references

benchmark: configure
	cmake --build $(BUILD_DIR) --target vector_benchmark

run-api: api
	./$(BUILD_DIR)/fraud_api

run-preprocess: preprocess
	if command -v gtime >/dev/null 2>&1; then \
		gtime -v ./$(BUILD_DIR)/preprocess_references; \
	elif [ -x /usr/bin/time ]; then \
		/usr/bin/time -v ./$(BUILD_DIR)/preprocess_references; \
	else \
		time ./$(BUILD_DIR)/preprocess_references; \
	fi

run-benchmark: benchmark
	./$(BUILD_DIR)/vector_benchmark

clean:
	cmake -E rm -rf $(BUILD_DIR)
