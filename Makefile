CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2
SRCS      = Tests/FormatTests.cpp Sources/USBAudioFormat.cpp

# Every source the formatter and the analyzer read.
C_SOURCES = Driver/GT10Audio.c Sources/GT10Ring.c Sources/GT10Clock.c \
            Tests/PlugInTests.c Tests/FakeUSB.c Tests/RingTests.c Tests/ClockTests.c \
            Tests/StreamTests.c Tests/USBDeviceTests.c Tests/usb_probe.c Tests/hal_probe.c
CXX_SOURCES = Driver/GT10USBDevice.cpp Driver/GT10USBStream.cpp Driver/GT10StreamCore.cpp \
              Sources/USBAudioFormat.cpp Tests/FormatTests.cpp
HEADERS = Driver/GT10StreamCore.h Driver/GT10USBDevice.h Driver/GT10USBInternal.h \
          Driver/GT10USBStream.h Sources/GT10Clock.h Sources/GT10Ring.h \
          Sources/USBAudioFormat.h

.PHONY: help check test clean fmt fmt-check analyze

help:
	@echo "check          every gate below, which is what CI runs"
	@echo "test           all suites, under AddressSanitizer and UndefinedBehaviorSanitizer"
	@echo "fmt            rewrite the sources in the project style"
	@echo "fmt-check      fail when a source is not in the project style"
	@echo "analyze        run the clang static analyzer"
	@echo "driver         build the plug-in bundle"
	@echo "driver-install install it and restart coreaudiod (needs sudo)"
	@echo "driver-check   install, then report whether it loaded"
	@echo "usb-probe      talk to the pedal directly (see README, it can panic the kernel)"
	@echo "hal-probe      counters from inside the running plug-in"

check: fmt-check analyze test

fmt:
	xcrun clang-format -i $(C_SOURCES) $(CXX_SOURCES) $(HEADERS)

fmt-check:
	xcrun clang-format --dry-run --Werror $(C_SOURCES) $(CXX_SOURCES) $(HEADERS)

# The analyzer is part of the clang that ships with Xcode, so this needs no
# install. It compiles each file on its own and links nothing.
analyze:
	@for f in $(C_SOURCES); do \
		$(CC) --analyze -Werror -Xanalyzer -analyzer-output=text -std=c11 $$f -o /dev/null || exit 1; \
	done
	@for f in $(CXX_SOURCES); do \
		$(CXX) --analyze -Werror -Xanalyzer -analyzer-output=text -std=c++17 $$f -o /dev/null || exit 1; \
	done
	@echo "static analyzer: clean"

# Tests build with ASan and UBSan, so an out-of-bounds read fails the run
# instead of reading a neighboring field.
SAN = -fsanitize=address,undefined -fno-sanitize-recover=all

# Always relinks rather than comparing timestamps. File mtime here has one
# second granularity, so an incremental rule reuses a stale binary whenever a
# source changes in the same second as the previous build. That happens on
# every mutation probe, where a stale binary reads as a passing test.
test:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SAN) -o build/formattests $(SRCS)
	./build/formattests
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -c -o build/gt10audio.o Driver/GT10Audio.c
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -c -o build/plugintests.o Tests/PlugInTests.c
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -c -o build/fakeusb.o Tests/FakeUSB.c
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -c -o build/ring.o Sources/GT10Ring.c
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -c -o build/clock.o Sources/GT10Clock.c
	$(CC)  $(SAN) -o build/plugintests build/plugintests.o build/gt10audio.o build/fakeusb.o \
		build/ring.o build/clock.o -framework CoreFoundation -framework CoreAudio
	./build/plugintests
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -c -o build/usbtests.o Tests/USBDeviceTests.c
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 $(SAN) -c -o build/gt10usb.o Driver/GT10USBDevice.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 $(SAN) -c -o build/stream.o Driver/GT10USBStream.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 $(SAN) -c -o build/streamcore.o Driver/GT10StreamCore.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 $(SAN) -c -o build/format.o Sources/USBAudioFormat.cpp
	$(CXX) $(SAN) -o build/usbtests build/usbtests.o build/gt10usb.o build/stream.o \
		build/streamcore.o build/format.o build/ring.o build/clock.o \
		-framework CoreFoundation -framework IOKit
	./build/usbtests
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -o build/ringtests Tests/RingTests.c Sources/GT10Ring.c
	./build/ringtests
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -c -o build/streamtests.o Tests/StreamTests.c
	$(CXX) $(SAN) -o build/streamtests build/streamtests.o build/streamcore.o build/gt10usb.o \
		build/ring.o build/clock.o build/stream.o build/format.o -framework CoreFoundation -framework IOKit
	./build/streamtests
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 $(SAN) -o build/clocktests Tests/ClockTests.c Sources/GT10Clock.c
	./build/clocktests

clean:
	rm -rf build

# --- AudioServerPlugIn ---------------------------------------------------
# Signing identity for the plug-in. Ad-hoc by default, which needs no keychain
# entry. Override with a self-signed identity once one exists.
DRIVER_SIGN_IDENTITY ?= -
BUNDLE                = build/GT10Audio.driver

.PHONY: driver driver-install driver-uninstall usb-probe usb-probe-build hal-probe

driver:
	@mkdir -p $(BUNDLE)/Contents/MacOS
	cp Driver/Info.plist $(BUNDLE)/Contents/Info.plist
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 -c -o $(BUNDLE)/gt10audio.o Driver/GT10Audio.c
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o $(BUNDLE)/gt10usb.o Driver/GT10USBDevice.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o $(BUNDLE)/stream.o Driver/GT10USBStream.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o $(BUNDLE)/core.o Driver/GT10StreamCore.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o $(BUNDLE)/format.o Sources/USBAudioFormat.cpp
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 -c -o $(BUNDLE)/ring.o Sources/GT10Ring.c
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 -c -o $(BUNDLE)/clock.o Sources/GT10Clock.c
	$(CXX) -bundle -o $(BUNDLE)/Contents/MacOS/GT10Audio \
		$(BUNDLE)/gt10audio.o $(BUNDLE)/gt10usb.o $(BUNDLE)/stream.o $(BUNDLE)/core.o \
		$(BUNDLE)/format.o $(BUNDLE)/ring.o $(BUNDLE)/clock.o \
		-framework CoreFoundation -framework CoreAudio -framework IOKit
	rm -f $(BUNDLE)/*.o
	codesign --force --sign "$(DRIVER_SIGN_IDENTITY)" --timestamp=none $(BUNDLE)
	codesign -dv --verbose=2 $(BUNDLE) 2>&1 | grep -E "Identifier|Signature|Authority|CDHash" || true

# Needs sudo: coreaudiod is a system daemon and reads only the system HAL
# directory, so a user-level bundle is never loaded. Restarting it cuts all
# audio on the machine for a moment.
#
# Signal the process rather than `launchctl kickstart`, which SIP refuses on a
# protected system service with "Operation not permitted". launchd brings
# coreaudiod straight back, and plug-ins are scanned on startup.
# Sign the installed copy in place. cp rewrites the executable's mtime, and an
# ad-hoc signature records the mtime it was signed at, so signing before the
# copy leaves cs_mtime != mtime and the kernel rejects the pages as tainted.
driver-install: driver
	sudo rm -rf /Library/Audio/Plug-Ins/HAL/GT10Audio.driver
	sudo cp -R $(BUNDLE) /Library/Audio/Plug-Ins/HAL/
	sudo codesign --force --sign "$(DRIVER_SIGN_IDENTITY)" --timestamp=none \
		/Library/Audio/Plug-Ins/HAL/GT10Audio.driver
	sudo killall coreaudiod

driver-uninstall:
	sudo rm -rf /Library/Audio/Plug-Ins/HAL/GT10Audio.driver
	sudo killall coreaudiod

# Install, restart, and report in one step. The reporting matters: a plug-in
# that fails to load does so silently, with no host process and no log line.
driver-check: driver-install
	@sleep 3
	@echo
	@echo "--- installed plist carries LoadingConditions? ---"
	@plutil -p /Library/Audio/Plug-Ins/HAL/GT10Audio.driver/Contents/Info.plist 2>/dev/null | head -3
	@echo "--- per-plugin host processes ---"
	@pgrep -fl "Core Audio Driver" || echo "  none"
	@echo "--- factory log ---"
	@log show --last 1m --predicate 'eventMessage CONTAINS "GT10Audio"' --info 2>/dev/null | tail -5 || true
	@echo
	@# Match without the parenthesis: pgrep takes an extended regex, so an
	@# unmatched "(" is a syntax error and reports a false negative.
	@pgrep -fl "Core Audio Driver" 2>/dev/null | grep -q GT10Audio \
		&& echo "VERDICT: loaded" \
		|| echo "VERDICT: not loaded"

# Live hardware probe. Separate from the unit-test target on purpose: it opens
# the real device. PROBE_ARGS selects the checkpoint, see Tests/usb_probe.c.
usb-probe-build:
	@mkdir -p build/probe
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o build/probe/usb.o Driver/GT10USBDevice.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o build/probe/stream.o Driver/GT10USBStream.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o build/probe/core.o Driver/GT10StreamCore.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -c -o build/probe/format.o Sources/USBAudioFormat.cpp
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 -c -o build/probe/ring.o Sources/GT10Ring.c
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 -c -o build/probe/clock.o Sources/GT10Clock.c
	$(CC)  -std=c11   -Wall -Wextra -Werror -O2 -c -o build/probe/probe.o Tests/usb_probe.c
	$(CXX) -o build/usbprobe build/probe/probe.o build/probe/usb.o build/probe/stream.o \
		build/probe/core.o build/probe/format.o build/probe/ring.o build/probe/clock.o \
		-framework CoreFoundation -framework IOKit

hal-probe:
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -o build/halprobe Tests/hal_probe.c \
		-framework CoreFoundation -framework CoreAudio
	./build/halprobe

usb-probe: usb-probe-build
	./build/usbprobe $(PROBE_ARGS)
