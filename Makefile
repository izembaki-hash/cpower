# CPower Pure C Windows-Native Computer-Use Agent Makefile
# Works with mingw32-make (GNU Make 4.4.1+) and MinGW-w64 GCC 16.1.0+

SHELL := cmd.exe

CC      := gcc
WINDRES := windres
OBJDUMP := objdump

CFLAGS      := -m64 -Wall -Wextra -Werror -pedantic -std=c11 -O2 \
               -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -Iinclude
TEST_CFLAGS := $(CFLAGS) -Wno-stringop-truncation
WINDRES_FLAGS := -F pe-x86-64
LDFLAGS     := -m64 -s -static-libgcc -Wl,--dynamicbase -Wl,--high-entropy-va -Wl,--nxcompat
LIBS    := -lole32 -loleaut32 -luiautomationcore -luser32 -lgdi32 \
           -ld3d11 -ldxgi -ladvapi32 -lm

CORE_SRCS := \
	src/common/core.c \
	src/common/logger.c \
	src/common/timer.c \
	src/perception/uia_tree.c \
	src/perception/winevent_hook.c \
	src/perception/fallback_text.c \
	src/perception/dxgi_capture.c \
	src/perception/tree_diff.c \
	src/input/direct_control.c \
	src/input/bezier_mouse.c \
	src/input/human_keyboard.c \
	src/input/window_ctrl.c \
	src/ipc/shm_transport.c \
	src/ipc/pipe_transport.c \
	src/security/privilege.c \
	src/hooks/hook_injector.c

CORE_OBJS := $(patsubst %.c,obj/%.o,$(notdir $(CORE_SRCS)))

MAIN_OBJ  := obj/main.o
RES_DEV   := obj/app_dev.res.o
RES_UIA   := obj/app.res.o

BIN_DEV   := bin/cpower.exe
BIN_UIA   := bin/cpower_uiaccess.exe
BIN_HOOK  := bin/cpower_hook64.dll
TEST_BIN  := tests/test_runner.exe

# Optional test suites detected dynamically
TEST_SUITE_CFLAGS :=
TEST_SUITE_OBJS   :=

ifneq ($(wildcard tests/test_perception.c),)
TEST_SUITE_CFLAGS += -DENABLE_PERCEPTION_TESTS
TEST_SUITE_OBJS   += obj/test_perception.o
endif
ifneq ($(wildcard tests/test_input.c),)
TEST_SUITE_CFLAGS += -DENABLE_INPUT_TESTS
TEST_SUITE_OBJS   += obj/test_input.o
endif
ifneq ($(wildcard tests/test_ipc.c),)
TEST_SUITE_CFLAGS += -DENABLE_IPC_TESTS
TEST_SUITE_OBJS   += obj/test_ipc.o
endif
ifneq ($(wildcard tests/test_security.c),)
TEST_SUITE_CFLAGS += -DENABLE_SECURITY_TESTS
TEST_SUITE_OBJS   += obj/test_security.o
endif
ifneq ($(wildcard tests/test_arch.c),)
TEST_SUITE_CFLAGS += -DENABLE_ARCH_TESTS
TEST_SUITE_OBJS   += obj/test_arch.o
endif
ifneq ($(wildcard tests/test_e2e.c),)
TEST_SUITE_CFLAGS += -DENABLE_E2E_TESTS
TEST_SUITE_OBJS   += obj/test_e2e.o
endif

VPATH := src:src/common:src/perception:src/input:src/ipc:src/security:src/hooks:tests

.PHONY: all cpower uiaccess hook64 tests audit verify test clean dirs

all: dirs cpower uiaccess hook64 tests audit

dirs:
	@if not exist obj mkdir obj
	@if not exist bin mkdir bin

obj/test_runner.o: tests/test_runner.c tests/test_infra.h | dirs
	$(CC) $(TEST_CFLAGS) -Itests $(TEST_SUITE_CFLAGS) -c $< -o $@

obj/test_%.o: tests/test_%.c tests/test_infra.h | dirs
	$(CC) $(TEST_CFLAGS) -Itests -c $< -o $@

obj/%.o: %.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(RES_DEV): res/app_dev.rc res/app_dev.manifest | dirs
	$(WINDRES) $(WINDRES_FLAGS) -Ires -i res/app_dev.rc -o $@

$(RES_UIA): res/app.rc res/app.manifest | dirs
	$(WINDRES) $(WINDRES_FLAGS) -Ires -i res/app.rc -o $@

cpower: $(BIN_DEV)

$(BIN_DEV): $(MAIN_OBJ) $(CORE_OBJS) $(RES_DEV)
	$(CC) -municode $(LDFLAGS) $^ -o $@ $(LIBS)

uiaccess: $(BIN_UIA)

$(BIN_UIA): $(MAIN_OBJ) $(CORE_OBJS) $(RES_UIA)
	$(CC) -municode $(LDFLAGS) $^ -o $@ $(LIBS)

hook64: $(BIN_HOOK)

$(BIN_HOOK): src/hooks/gdi_dwrite_hook.c include/text_hook.h include/common.h | dirs
	$(CC) $(CFLAGS) -shared $(LDFLAGS) src/hooks/gdi_dwrite_hook.c -o $@ -luser32 -lgdi32 -ladvapi32

tests: $(TEST_BIN)

$(TEST_BIN): obj/test_runner.o $(TEST_SUITE_OBJS) $(CORE_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

audit: $(BIN_DEV) $(BIN_UIA) $(BIN_HOOK) $(TEST_BIN)
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "$$ErrorActionPreference='Stop'; $$re='^(KERNEL32|USER32|GDI32|ADVAPI32|ole32|OLEAUT32|UIAutomationCore|dxgi|d3d11|api-ms-win-crt-.*|ucrtbase|msvcrt)\.dll$$'; foreach ($$b in @('$(BIN_DEV)','$(BIN_UIA)','$(BIN_HOOK)','$(TEST_BIN)')) { Write-Host ('=== DLL Import Audit: ' + $$b + ' ==='); $$lines = & '$(OBJDUMP)' -p $$b; if ($$LASTEXITCODE -ne 0) { exit $$LASTEXITCODE }; $$dlls = ($$lines | Select-String 'DLL Name:\s+(\S+)' | ForEach-Object { $$_.Matches[0].Groups[1].Value }); if (-not $$dlls) { Write-Error ('No DLL imports parsed from ' + $$b); exit 1 }; foreach ($$d in $$dlls) { Write-Host ('  ' + $$d); if ($$d -notmatch $$re) { Write-Error ('Prohibited/non-OS DLL dependency in ' + $$b + ': ' + $$d); exit 1 } } }"

verify: $(BIN_DEV)
	$(BIN_DEV) verify

test: $(TEST_BIN)
	$(TEST_BIN) --all

clean:
	@if exist obj\*.o del /Q obj\*.o
	@if exist obj\*.res del /Q obj\*.res
	@if exist bin\*.exe del /Q bin\*.exe
	@if exist bin\*.dll del /Q bin\*.dll
	@if exist tests\*.exe del /Q tests\*.exe
