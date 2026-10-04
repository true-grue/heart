# GNU Make + GCC. Linux — нативно, Windows — через MSYS2/MinGW с тем же GCC.
# wasm пока не настроен: это тот же Makefile с CC=emcc, появится вместе с io_backend_web.c.

CC := gcc
AR := ar

CSTD := -std=c11
WARN := -pedantic -Wall -Wextra -Werror

REL     := -O2
DBG     := -O0 -g
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer
ANALYZE := -fanalyzer

INC := -Isrc

BUILD := build
OBJ   := $(BUILD)/obj
TOBJ  := $(BUILD)/tobj
AOBJ  := $(BUILD)/an
DOBJ  := $(BUILD)/dobj

# Списки исходников явные: детерминированный порядок линковки, без $(shell find).
LIB_SRC  := src/arena.c \
            src/utf8.c \
            src/dsl.c \
            src/game.c \
            src/io.c \
            src/test_platform.c \
            src/font.c

# The X11 backend is the only part outside the engine that links a system library.
# Windows and Web backends will use their own platform interfaces instead.
X11_SRC  := src/x11_platform.c
X11_FLAGS := -DIO_X11
X11_LIBS := -lX11
HAVE_X11 := $(shell $(CC) -x c -include X11/Xlib.h -E /dev/null 2>/dev/null && echo 1)

# The target is chosen by the compiler, not by the caller: a MinGW GCC is a Windows
# build and needs no other switch, which is the whole point of the one-line seam.
ifneq (,$(findstring mingw,$(CC)))
LIB_SRC   += src/win_platform.c
CFLAGS_X  := -DIO_WIN
# winpthread carries clock_gettime, which is what the application asks for on every
# platform. Some MinGW builds link it implicitly and some do not, so it is named.
#
# And it is linked statically on purpose. By default the executable imports
# libwinpthread-1.dll and refuses to start without it beside it, so every copy of the
# game has to carry a DLL along. Static costs about 60 kB and the game is one file that
# cannot lose its neighbour. -static-libwinpthread does not exist in this GCC, so the
# archive is picked out by hand.
LDFLAGS_X := -lgdi32 -luser32 -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
else ifeq ($(HAVE_X11),1)
LIB_SRC   += $(X11_SRC)
CFLAGS_X  := $(X11_FLAGS)
LDFLAGS_X := $(X11_LIBS)
endif

HDRS := src/arena.h \
        src/utf8.h \
        src/game.h \
        src/io.h \
        src/dsl.h \
        src/test_platform.h \
        src/font.h

STRESS_SRC := tests/stress.c
QUEST_SRC := src/main.c
STRESS   := $(BUILD)/stress
QUEST    := $(BUILD)/quest
TOOL_SRC := tools/test.c
TEST_INC := -Itools
TEST_SRC := tools/test_main.c tests/tests.c
ALL_SRC  := $(LIB_SRC) $(TOOL_SRC) $(TEST_SRC)

LIB_SRC   := $(strip $(LIB_SRC))
LIB_OBJ  := $(LIB_SRC:%.c=$(OBJ)/%.o)
TEST_OBJ := $(ALL_SRC:%.c=$(TOBJ)/%.o)
AN_OBJ   := $(ALL_SRC:%.c=$(AOBJ)/%.o)

LIB       := $(BUILD)/libquest.a
TEST_BIN  := $(BUILD)/tests
STRESS_ASAN := $(BUILD)/stress-asan
QUEST_ASAN  := $(BUILD)/quest-asan

# A stamp carrying the toolchain and the flags. Without it, switching CC or a
# flag silently reused objects and a binary linked against another runtime: the
# demo survived once as a stale executable needing a libasan that no longer
# existed, and make reported "nothing to be done".
CONFIG := $(BUILD)/.config-$(notdir $(CC))-$(CSTD)

$(CONFIG):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.config-*
	@printf '%s\n' '$(CC) $(CSTD) $(WARN) $(REL) $(DBG) $(SAN) $(ANALYZE) $(INC) $(CFLAGS_X) $(LDFLAGS_X)' > $@

DEP := $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(AN_OBJ:.o=.d)

.PHONY: all test analyze demo demo-asan win clean

all: $(LIB)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(OBJ)/%.o: %.c $(CONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(REL) $(INC) $(CFLAGS_X) -MMD -MP -c -o $@ $<

$(TOBJ)/%.o: %.c $(CONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(DBG) $(SAN) $(INC) $(TEST_INC) -MMD -MP -c -o $@ $<

$(AOBJ)/%.o: %.c $(CONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(DBG) $(ANALYZE) $(INC) -MMD -MP -c -o $@ $<

$(TEST_BIN): $(TEST_OBJ)
	$(CC) $(CSTD) $(WARN) $(DBG) $(SAN) -o $@ $^

# The two interactive programs, built without sanitizers: no ASan runtime to ship
# into something a person actually plays with.
$(STRESS): $(LIB_SRC) $(STRESS_SRC) $(HDRS) $(CONFIG)
	$(CC) $(CSTD) $(WARN) $(REL) $(INC) $(CFLAGS_X) -o $@ $(LIB_SRC) $(STRESS_SRC) $(LDFLAGS_X)

$(QUEST): $(LIB_SRC) $(QUEST_SRC) $(HDRS) $(CONFIG)
	$(CC) $(CSTD) $(WARN) $(REL) $(INC) $(CFLAGS_X) -o $@ $(LIB_SRC) $(QUEST_SRC) $(LDFLAGS_X)

# The same two under ASan, for hunting bugs in the interactive layer.
$(STRESS_ASAN): $(LIB_SRC) $(STRESS_SRC) $(HDRS) $(CONFIG)
	$(CC) $(CSTD) $(WARN) $(DBG) $(SAN) $(INC) $(CFLAGS_X) -o $@ $(LIB_SRC) $(STRESS_SRC) $(LDFLAGS_X)

$(QUEST_ASAN): $(LIB_SRC) $(QUEST_SRC) $(HDRS) $(CONFIG)
	$(CC) $(CSTD) $(WARN) $(DBG) $(SAN) $(INC) $(CFLAGS_X) -o $@ $(LIB_SRC) $(QUEST_SRC) $(LDFLAGS_X)

# Windows is chosen by the compiler; this target is only so that the compiler does not
# have to be remembered. Nothing else about the build changes.
win:
	$(MAKE) CC=x86_64-w64-mingw32-gcc quest

demo: $(STRESS)
demo-asan: $(STRESS_ASAN)
quest: $(QUEST)
quest-asan: $(QUEST_ASAN)

test: $(TEST_BIN)
	./$(TEST_BIN)

analyze: $(AN_OBJ)

clean:
	rm -rf $(BUILD)

-include $(DEP)