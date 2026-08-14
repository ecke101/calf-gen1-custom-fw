CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Werror -pedantic
CPPFLAGS = -Iinclude
BUILD_DIR = ../build/ui
FIRMWARE ?= VIEWPT-REALIA-V2.2.1-20260626-UPDATE.tar.gz
SANITIZE_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
FONT_SOURCE = $(BUILD_DIR)/ui_font_data.c
FONT_ASSETS = assets/fonts/NotoSans-Regular.ttf \
	assets/fonts/NotoSansSymbols-Regular.ttf \
	assets/fonts/NotoSansSymbols2-Regular.ttf
UI_SOURCES = src/ui.c src/ui_input.c src/ui_render.c src/ui_font.c \
	src/ui_locale.c src/sha256.c $(FONT_SOURCE)
TEST_EXTRA_SOURCES = src/target_capture.c src/target_power.c

.PHONY: all desktop test sanitize sanitize-desktop sanitize-smoke sanitize-test \
	target clean

all: desktop test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(FONT_SOURCE): $(FONT_ASSETS) ../src/calf_fw_tool/font_embed.py | $(BUILD_DIR)
	PYTHONPATH=../src $(PYTHON) -m calf_fw_tool.font_embed $@ \
		calf_font_noto_sans=assets/fonts/NotoSans-Regular.ttf \
		calf_font_noto_symbols=assets/fonts/NotoSansSymbols-Regular.ttf \
		calf_font_noto_symbols2=assets/fonts/NotoSansSymbols2-Regular.ttf

desktop: $(BUILD_DIR) $(FONT_SOURCE)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		$$(pkg-config --cflags sdl2) $(UI_SOURCES) src/desktop.c \
		$$(pkg-config --libs sdl2) -lm -o $(BUILD_DIR)/calf-ui-desktop

test: $(BUILD_DIR) $(FONT_SOURCE)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		$(UI_SOURCES) $(TEST_EXTRA_SOURCES) tests/test_ui.c -lm \
		-o $(BUILD_DIR)/test-ui
	$(BUILD_DIR)/test-ui
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) -Isrc \
		-ffunction-sections -fdata-sections src/target_gallery.c \
		tests/test_target_gallery.c -Wl,--gc-sections \
		-o $(BUILD_DIR)/test-target-gallery
	$(BUILD_DIR)/test-target-gallery

sanitize: sanitize-test sanitize-desktop sanitize-smoke

sanitize-test: $(BUILD_DIR) $(FONT_SOURCE)
	$(CC) -std=c11 $(SANITIZE_FLAGS) $(WARNINGS) $(CPPFLAGS) \
		$(UI_SOURCES) $(TEST_EXTRA_SOURCES) tests/test_ui.c \
		-lm -o $(BUILD_DIR)/test-ui-sanitize
	$(BUILD_DIR)/test-ui-sanitize
	$(CC) -std=c11 $(SANITIZE_FLAGS) $(WARNINGS) $(CPPFLAGS) -Isrc \
		-ffunction-sections -fdata-sections src/target_gallery.c \
		tests/test_target_gallery.c -Wl,--gc-sections \
		-o $(BUILD_DIR)/test-target-gallery-sanitize
	$(BUILD_DIR)/test-target-gallery-sanitize

sanitize-desktop: $(BUILD_DIR) $(FONT_SOURCE)
	$(CC) -std=c11 $(SANITIZE_FLAGS) $(WARNINGS) $(CPPFLAGS) \
		$$(pkg-config --cflags sdl2) $(UI_SOURCES) src/desktop.c \
		$$(pkg-config --libs sdl2) -lm \
		-o $(BUILD_DIR)/calf-ui-desktop-sanitize

sanitize-smoke: sanitize-desktop
	ASAN_OPTIONS=detect_leaks=1 SDL_VIDEODRIVER=dummy \
		$(BUILD_DIR)/calf-ui-desktop-sanitize --smoke-test

target: $(BUILD_DIR)
	cd .. && PYTHONPATH=src $(PYTHON) -m calf_fw_tool build-ui \
		$(FIRMWARE) --ui-source ui --output build/ui/calf-ui-aarch64

clean:
	rm -f $(BUILD_DIR)/calf-ui-desktop $(BUILD_DIR)/test-ui \
		$(BUILD_DIR)/calf-ui-desktop-sanitize \
		$(BUILD_DIR)/test-ui-sanitize \
		$(BUILD_DIR)/test-target-gallery \
		$(BUILD_DIR)/test-target-gallery-sanitize \
		$(BUILD_DIR)/calf-ui-aarch64 \
		$(FONT_SOURCE) $(FONT_SOURCE).tmp
