.PHONY: all clean run

BUILD_DIR := build/mcu-save-transfer
CONFIG ?= Release

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)
	cmake --build $(BUILD_DIR) --config $(CONFIG)

run: all
	./$(BUILD_DIR)/MCU_Save_Transfer

clean:
	cmake -E rm -rf build bin
