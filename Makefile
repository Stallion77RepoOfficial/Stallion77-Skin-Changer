PREFIX ?= /opt/homebrew
BUILD ?= build
ARCH ?= arm64

IMGUI_VERSION ?= v1.91.9b
IMGUI_DIR := $(BUILD)/imgui
CXXFLAGS := -std=c++17 -O2 -I./src -I$(PREFIX)/include -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
LDFLAGS := -L$(PREFIX)/lib -lglfw -framework Cocoa -framework OpenGL -framework IOKit -framework ApplicationServices
IMGUI_CPP := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_BACKENDS := $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

.PHONY: all imgui clean offsets FORCE

all: imgui $(BUILD)/StallionSkinChanger $(BUILD)/stallion-core

imgui:
	@test -d $(IMGUI_DIR) || (mkdir -p $(BUILD) && git clone --depth 1 --branch $(IMGUI_VERSION) https://github.com/ocornut/imgui.git $(IMGUI_DIR))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/StallionSkinChanger: FORCE imgui $(BUILD)
	$(CXX) $(CXXFLAGS) -x objective-c++ src/main.mm $(IMGUI_CPP) $(IMGUI_BACKENDS) $(LDFLAGS) -o $@

$(BUILD)/stallion-core: FORCE $(BUILD)
	$(CXX) $(CXXFLAGS) src/core.cpp src/live_skin.cpp -o $@ -lproc
	printf '<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>com.apple.security.cs.debugger</key><true/></dict></plist>' > $(BUILD)/core.entitlements
	codesign -s - -f --entitlements $(BUILD)/core.entitlements $@

offsets: all
	python3 tools/scan_offsets.py --output offsets.json

clean:
	rm -rf $(BUILD)

FORCE:
