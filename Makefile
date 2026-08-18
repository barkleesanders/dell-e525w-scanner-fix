SHELL := /bin/bash

.PHONY: all check install clean

all:
	@DELL_SCAN_SOURCE_DIR="$(CURDIR)" bash install.sh

check:
	@bash scripts/check.sh

install: all

clean:
	@if [[ -d build ]]; then find build -type f -delete; rmdir build; fi
