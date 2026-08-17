CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Werror -pedantic -Wno-misleading-indentation \
	-Wno-unused-but-set-variable
AAC_DIR = vendor/vo-aacenc
AAC_SOURCES = $(AAC_DIR)/common/cmnMemory.c \
	$(wildcard $(AAC_DIR)/aacenc/basic_op/*.c) \
	$(wildcard $(AAC_DIR)/aacenc/src/*.c)
XAAC_DIR = vendor/libxaac
XAAC_SOURCES = $(wildcard $(XAAC_DIR)/common/*.c) \
	$(wildcard $(XAAC_DIR)/decoder/*.c) \
	$(wildcard $(XAAC_DIR)/decoder/drc_src/*.c) \
	$(XAAC_DIR)/decoder/x86/ixheaacd_function_selector_x86.c \
	$(XAAC_DIR)/decoder/generic/ixheaacd_qmf_dec_generic.c
CPPFLAGS = -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DUSE_DEFAULT_STDLIB -D__unused= -Iinclude \
	-I$(AAC_DIR)/common/include -I$(AAC_DIR)/aacenc/basic_op \
	-I$(AAC_DIR)/aacenc/inc -I$(XAAC_DIR)/common \
	-I$(XAAC_DIR)/decoder -I$(XAAC_DIR)/decoder/drc_src \
	-I$(XAAC_DIR)/decoder/generic -I$(XAAC_DIR)/decoder/x86
BUILD_DIR = ../build/ngcd
SOURCES = src/app.c src/api.c src/audio.c src/aac.c src/aac_decoder.c src/backlight.c src/backend_mock.c src/backend_target.c \
	src/http.c src/json.c src/power.c src/profile.c src/rockchip_graph.c \
	src/raw_stack.c \
	src/system.c \
	src/rockchip_display.c src/rockchip_encoder.c src/rockchip_image.c src/mp4.c \
	src/mp4_reader.c src/rockchip_audio.c src/rockchip_playback.c \
	src/rockchip_target.c src/imu.c \
	src/storage.c src/wifi.c src/ethernet.c src/usb_ethernet.c $(AAC_SOURCES)
LDLIBS ?= -ldl -pthread -lm
SANITIZE_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
AAC_SANITIZE_DIR = $(BUILD_DIR)/vo-aacenc-sanitize
AAC_SANITIZE_OBJECTS = $(patsubst $(AAC_DIR)/%.c,$(AAC_SANITIZE_DIR)/%.o,$(AAC_SOURCES))
XAAC_OBJECTS = $(patsubst $(XAAC_DIR)/%.c,$(BUILD_DIR)/libxaac/%.o,$(XAAC_SOURCES))
XAAC_SANITIZE_OBJECTS = $(patsubst $(XAAC_DIR)/%.c,$(BUILD_DIR)/libxaac-sanitize/%.o,$(XAAC_SOURCES))

.PHONY: all target test sanitize clean

all: $(BUILD_DIR)/ngcd

target:
	cd .. && PYTHONPATH=src $(PYTHON) -m calf_fw_tool build-ngcd \
		--ngcd-source ngcd --output build/ngcd/ngcd-aarch64

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(AAC_SANITIZE_DIR)/%.o: $(AAC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O1 -g $(WARNINGS) $(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/libxaac/%.o: $(XAAC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -fwrapv -w -DX86_64 -D_X86_64_ \
		$(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/libxaac-sanitize/%.o: $(XAAC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 $(SANITIZE_FLAGS) -fno-sanitize=undefined \
		-fwrapv -w -DX86_64 -D_X86_64_ $(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/libxaacdec.a: $(XAAC_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/libxaacdec-sanitize.a: $(XAAC_SANITIZE_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/ngcd: $(SOURCES) src/main.c include/ngcd.h $(BUILD_DIR)/libxaacdec.a | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		$(SOURCES) src/main.c $(BUILD_DIR)/libxaacdec.a $(LDLIBS) -o $@

$(BUILD_DIR)/test-ngcd: $(SOURCES) tests/test_ngcd.c include/ngcd.h $(BUILD_DIR)/libxaacdec.a | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		$(SOURCES) tests/test_ngcd.c $(BUILD_DIR)/libxaacdec.a $(LDLIBS) -o $@

$(BUILD_DIR)/check-profile: src/profile.c tests/check_profile.c include/ngcd.h | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/profile.c tests/check_profile.c -o $@

$(BUILD_DIR)/test-rockchip-graph: src/rockchip_graph.c src/rockchip_encoder.c src/mp4.c src/aac.c $(AAC_SOURCES) \
		tests/test_rockchip_graph.c \
		include/ngcd.h include/ngcd_rk.h | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/rockchip_graph.c src/rockchip_encoder.c src/mp4.c src/aac.c $(AAC_SOURCES) \
		tests/test_rockchip_graph.c -o $@

$(BUILD_DIR)/test-rockchip-display: src/rockchip_display.c \
		tests/test_rockchip_display.c include/ngcd.h include/ngcd_rk.h | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/rockchip_display.c tests/test_rockchip_display.c -o $@

$(BUILD_DIR)/test-rockchip-playback: src/rockchip_playback.c src/rockchip_audio.c src/mp4.c src/aac.c src/aac_decoder.c $(AAC_SOURCES) $(BUILD_DIR)/libxaacdec.a \
        src/mp4_reader.c tests/test_rockchip_playback.c \
		include/ngcd.h include/ngcd_rk.h include/ngcd_playback.h | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
	src/rockchip_playback.c src/rockchip_audio.c src/mp4.c src/aac.c src/aac_decoder.c $(AAC_SOURCES) $(BUILD_DIR)/libxaacdec.a src/mp4_reader.c \
		tests/test_rockchip_playback.c -lm -o $@

$(BUILD_DIR)/test-mp4: src/mp4.c src/aac.c src/aac_decoder.c $(AAC_SOURCES) $(BUILD_DIR)/libxaacdec.a src/mp4_reader.c tests/test_mp4.c \
		include/ngcd_mp4.h include/ngcd_playback.h | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/mp4.c src/aac.c src/aac_decoder.c $(AAC_SOURCES) $(BUILD_DIR)/libxaacdec.a src/mp4_reader.c tests/test_mp4.c -lm -o $@

$(BUILD_DIR)/test-imu: src/imu.c tests/test_imu.c include/ngcd.h \
		include/ngcd_imu.h | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/imu.c tests/test_imu.c -pthread -o $@

$(BUILD_DIR)/test-raw-stack: src/raw_stack.c tests/test_raw_stack.c \
		include/ngcd_raw.h | $(BUILD_DIR)
	$(CC) -std=c11 $(CFLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/raw_stack.c tests/test_raw_stack.c -o $@

test: $(BUILD_DIR)/ngcd $(BUILD_DIR)/test-ngcd $(BUILD_DIR)/check-profile \
		$(BUILD_DIR)/test-rockchip-graph $(BUILD_DIR)/test-rockchip-display \
		$(BUILD_DIR)/test-rockchip-playback $(BUILD_DIR)/test-mp4 \
		$(BUILD_DIR)/test-imu $(BUILD_DIR)/test-raw-stack
	$(BUILD_DIR)/test-ngcd
	$(BUILD_DIR)/test-rockchip-graph
	$(BUILD_DIR)/test-rockchip-display
	$(BUILD_DIR)/test-rockchip-playback
	$(BUILD_DIR)/test-mp4
	$(BUILD_DIR)/test-imu
	$(BUILD_DIR)/test-raw-stack
	$(BUILD_DIR)/check-profile profiles/ngcd-vr180-5k60.yaml
	$(PYTHON) tests/test_http.py

sanitize: $(AAC_SANITIZE_OBJECTS) $(BUILD_DIR)/libxaacdec-sanitize.a | $(BUILD_DIR)
	$(CC) -std=c11 $(SANITIZE_FLAGS) $(WARNINGS) $(CPPFLAGS) \
		$(filter-out $(AAC_SOURCES),$(SOURCES)) \
		$(AAC_SANITIZE_OBJECTS) $(BUILD_DIR)/libxaacdec-sanitize.a \
		tests/test_ngcd.c $(LDLIBS) -o $(BUILD_DIR)/test-ngcd-sanitize
	$(BUILD_DIR)/test-ngcd-sanitize
	$(CC) -std=c11 $(SANITIZE_FLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/mp4.c src/aac.c src/aac_decoder.c $(AAC_SANITIZE_OBJECTS) \
		$(BUILD_DIR)/libxaacdec-sanitize.a src/mp4_reader.c tests/test_mp4.c \
		-lm -o $(BUILD_DIR)/test-mp4-sanitize
	$(BUILD_DIR)/test-mp4-sanitize
	$(CC) -std=c11 $(SANITIZE_FLAGS) $(WARNINGS) $(CPPFLAGS) \
		src/imu.c tests/test_imu.c -pthread -o $(BUILD_DIR)/test-imu-sanitize
	$(BUILD_DIR)/test-imu-sanitize

clean:
	rm -f $(BUILD_DIR)/ngcd $(BUILD_DIR)/test-ngcd \
		$(BUILD_DIR)/test-ngcd-sanitize $(BUILD_DIR)/check-profile \
		$(BUILD_DIR)/test-rockchip-graph $(BUILD_DIR)/test-rockchip-display \
		$(BUILD_DIR)/test-rockchip-playback \
		$(BUILD_DIR)/test-mp4 $(BUILD_DIR)/test-mp4-sanitize \
		$(BUILD_DIR)/test-imu $(BUILD_DIR)/test-imu-sanitize \
		$(BUILD_DIR)/test-raw-stack \
		$(BUILD_DIR)/ngcd-aarch64 \
		$(BUILD_DIR)/libxaacdec.a $(BUILD_DIR)/libxaacdec-sanitize.a \
		$(XAAC_OBJECTS) $(XAAC_SANITIZE_OBJECTS)
