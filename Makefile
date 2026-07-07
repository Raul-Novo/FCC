# SPDX-License-Identifier: GPL-3.0-or-later
SHELL := cmd.exe
.SHELLFLAGS := /C

CC := clang-cl
INCLUDE_DIR := include
SRC_DIR := src
TEST_DIR := tests
SCRIPT_DIR := scripts
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

COMMON_CL_RSP := $(SCRIPT_DIR)/clang_cl_common.rsp
DEBUG_CL_RSP := $(SCRIPT_DIR)/clang_cl_debug.rsp
RELEASE_CL_RSP := $(SCRIPT_DIR)/clang_cl_release.rsp
ASAN_CL_RSP := $(SCRIPT_DIR)/clang_cl_asan.rsp
LINK_FLAGS := /link /nologo

FCC_CORE_UNITS := fcc_ast fcc_codegen fcc_diag fcc_driver fcc_layout fcc_lexer fcc_link fcc_parser fcc_preprocessor fcc_sema fcc_signature fcc_source fcc_symbol fcc_token fcc_type
DEBUG_CORE_OBJS := $(addprefix $(OBJ_DIR)/debug/,$(addsuffix .obj,$(FCC_CORE_UNITS)))
RELEASE_CORE_OBJS := $(addprefix $(OBJ_DIR)/release/,$(addsuffix .obj,$(FCC_CORE_UNITS)))
ASAN_CORE_OBJS := $(addprefix $(OBJ_DIR)/asan/,$(addsuffix .obj,$(FCC_CORE_UNITS)))
TEST_CORE_OBJS := $(addprefix $(OBJ_DIR)/tests/,$(addsuffix .obj,$(FCC_CORE_UNITS)))

DEBUG_OBJS := $(DEBUG_CORE_OBJS) $(OBJ_DIR)/debug/main.obj
RELEASE_OBJS := $(RELEASE_CORE_OBJS) $(OBJ_DIR)/release/main.obj
ASAN_OBJS := $(ASAN_CORE_OBJS) $(OBJ_DIR)/asan/main.obj
TEST_OBJS := $(TEST_CORE_OBJS) $(OBJ_DIR)/tests/test_main.obj

.PHONY: debug release asan test smoke selfhost-inventory selfhost clean

debug: $(BIN_DIR)/fcc.exe
	@echo Built $(BIN_DIR)\fcc.exe

release: $(BIN_DIR)/fcc_release.exe
	@echo Built $(BIN_DIR)\fcc_release.exe

asan: $(BIN_DIR)/fcc_asan.exe
	@echo Built $(BIN_DIR)\fcc_asan.exe

test: $(BIN_DIR)/fcc.exe $(BIN_DIR)/fcc_tests.exe
	"$(BIN_DIR)\fcc_tests.exe"

smoke: $(BIN_DIR)/fcc.exe
	powershell -NoProfile -ExecutionPolicy Bypass -File "$(SCRIPT_DIR)\smoke_codegen.ps1"

selfhost-inventory: $(BIN_DIR)/fcc.exe
	powershell -NoProfile -ExecutionPolicy Bypass -File "$(SCRIPT_DIR)\bootstrap_inventory.ps1"

selfhost: $(BIN_DIR)/fcc.exe
	powershell -NoProfile -ExecutionPolicy Bypass -File "$(SCRIPT_DIR)\bootstrap_selfhost.ps1"

clean:
	if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"

$(BIN_DIR)/fcc.exe: $(DEBUG_OBJS)
	if not exist "$(BIN_DIR)" mkdir "$(BIN_DIR)"
	$(CC) @$(COMMON_CL_RSP) @$(DEBUG_CL_RSP) $(DEBUG_OBJS) /Fe$@ $(LINK_FLAGS)

$(BIN_DIR)/fcc_release.exe: $(RELEASE_OBJS)
	if not exist "$(BIN_DIR)" mkdir "$(BIN_DIR)"
	$(CC) @$(COMMON_CL_RSP) @$(RELEASE_CL_RSP) $(RELEASE_OBJS) /Fe$@ $(LINK_FLAGS)

$(BIN_DIR)/fcc_asan.exe: $(ASAN_OBJS)
	if not exist "$(BIN_DIR)" mkdir "$(BIN_DIR)"
	$(CC) @$(COMMON_CL_RSP) @$(ASAN_CL_RSP) $(ASAN_OBJS) /Fe$@ $(LINK_FLAGS)

$(BIN_DIR)/fcc_tests.exe: $(TEST_OBJS)
	if not exist "$(BIN_DIR)" mkdir "$(BIN_DIR)"
	$(CC) @$(COMMON_CL_RSP) @$(DEBUG_CL_RSP) $(TEST_OBJS) /Fe$@ $(LINK_FLAGS)

$(OBJ_DIR)/debug/%.obj: $(SRC_DIR)/%.c $(COMMON_CL_RSP) $(DEBUG_CL_RSP)
	if not exist "$(OBJ_DIR)\debug" mkdir "$(OBJ_DIR)\debug"
	$(CC) @$(COMMON_CL_RSP) @$(DEBUG_CL_RSP) /c $< /Fo$@

$(OBJ_DIR)/release/%.obj: $(SRC_DIR)/%.c $(COMMON_CL_RSP) $(RELEASE_CL_RSP)
	if not exist "$(OBJ_DIR)\release" mkdir "$(OBJ_DIR)\release"
	$(CC) @$(COMMON_CL_RSP) @$(RELEASE_CL_RSP) /c $< /Fo$@

$(OBJ_DIR)/asan/%.obj: $(SRC_DIR)/%.c $(COMMON_CL_RSP) $(ASAN_CL_RSP)
	if not exist "$(OBJ_DIR)\asan" mkdir "$(OBJ_DIR)\asan"
	$(CC) @$(COMMON_CL_RSP) @$(ASAN_CL_RSP) /c $< /Fo$@

$(OBJ_DIR)/tests/%.obj: $(SRC_DIR)/%.c $(COMMON_CL_RSP) $(DEBUG_CL_RSP)
	if not exist "$(OBJ_DIR)\tests" mkdir "$(OBJ_DIR)\tests"
	$(CC) @$(COMMON_CL_RSP) @$(DEBUG_CL_RSP) /c $< /Fo$@

$(OBJ_DIR)/debug/main.obj: $(SRC_DIR)/main.c $(COMMON_CL_RSP) $(DEBUG_CL_RSP)
	if not exist "$(OBJ_DIR)\debug" mkdir "$(OBJ_DIR)\debug"
	$(CC) @$(COMMON_CL_RSP) @$(DEBUG_CL_RSP) /c $< /Fo$@

$(OBJ_DIR)/release/main.obj: $(SRC_DIR)/main.c $(COMMON_CL_RSP) $(RELEASE_CL_RSP)
	if not exist "$(OBJ_DIR)\release" mkdir "$(OBJ_DIR)\release"
	$(CC) @$(COMMON_CL_RSP) @$(RELEASE_CL_RSP) /c $< /Fo$@

$(OBJ_DIR)/asan/main.obj: $(SRC_DIR)/main.c $(COMMON_CL_RSP) $(ASAN_CL_RSP)
	if not exist "$(OBJ_DIR)\asan" mkdir "$(OBJ_DIR)\asan"
	$(CC) @$(COMMON_CL_RSP) @$(ASAN_CL_RSP) /c $< /Fo$@

$(OBJ_DIR)/tests/test_main.obj: $(TEST_DIR)/test_main.c $(COMMON_CL_RSP) $(DEBUG_CL_RSP)
	if not exist "$(OBJ_DIR)\tests" mkdir "$(OBJ_DIR)\tests"
	$(CC) @$(COMMON_CL_RSP) @$(DEBUG_CL_RSP) /c $< /Fo$@
