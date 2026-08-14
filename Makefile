PYTHON ?= python3
CLANG_TIDY ?= clang-tidy
C_LINT_SOURCES = \
	ui/src/sha256.c \
	ui/src/target.c \
	ui/src/target_capture.c \
	ui/src/target_gallery.c \
	ui/src/target_power.c \
	ui/src/target_platform.c \
	ui/src/target_storage.c \
	ui/src/target_update.c \
	ui/src/ui.c \
	ui/src/ui_input.c \
	ui/src/ui_render.c

.PHONY: test lint lint-python lint-c lint-license audit build release-check

test:
	PYTHONPATH=src $(PYTHON) -m pytest -q
	$(MAKE) -C ui test
	$(MAKE) -C ngcd test

lint: lint-python lint-c lint-license

lint-python:
	$(PYTHON) -m ruff check src tests tools

lint-c:
	$(CLANG_TIDY) $(C_LINT_SOURCES) -- -std=c11 -Iui/include -Iui/src

lint-license:
	reuse lint

audit:
	$(PYTHON) tools/audit_release.py

build:
	PYTHONPATH=src $(PYTHON) -m calf_fw_tool build

release-check: test lint audit build
