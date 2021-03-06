
# Don't use implicit rules or variables
# we have explicit rules for everything
MAKEFLAGS += -rR

# Files with this suffixes are final, don't try to generate them
# using implicit rules
%.d:
%.h:
%.c:
%.cpp:
%.m:
%.mak:

# Flags for dependency generation
QEMU_DGFLAGS += -MMD -MP -MT $@ -MF $(*D)/$(*F).d

ifdef CONFIG_ASAN

%.o: %.c $(GENERATED_HEADERS)
	$(call quiet-command,$(ASANCC) $(QEMU_INCLUDES) $(QEMU_CFLAGS)  $(QEMU_DGFLAGS) $(CFLAGS) $(ASAN_FLAGS) -c -o $@ $<,"  ASANCC    $(TARGET_DIR)$@")

%.o: %.cpp $(GENERATED_HEADERS)
	$(call quiet-command,$(ASANCXX) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_CXXFLAGS) $(QEMU_DGFLAGS) $(CXXFLAGS) $(ASAN_FLAGS) -c -o $@ $<,"  ASANCXX   $(TARGET_DIR)$@")

%.o: %.m
	$(call quiet-command,$(ASANCC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  OBJC  $(TARGET_DIR)$@")


else

%.o: %.c $(GENERATED_HEADERS)
	$(call quiet-command,$(LLVMCC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  CC    $(TARGET_DIR)$@")

%.o: %.cpp $(GENERATED_HEADERS)
	$(call quiet-command,$(LLVMCXX) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_CXXFLAGS) $(QEMU_DGFLAGS) $(CXXFLAGS) -c -o $@ $<,"  CXX   $(TARGET_DIR)$@")

%.o: %.m
	$(call quiet-command,$(LLVMCC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  OBJC  $(TARGET_DIR)$@")

endif

ifeq ($(LIBTOOL),)
%.lo: %.c
	@echo "missing libtool. please install and rerun configure"; exit 1
else
%.lo: %.c
	$(call quiet-command,$(LIBTOOL) --mode=compile --quiet --tag=CC $(CC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  lt CC $@")
endif


%.o: %.S
	$(call quiet-command,$(CC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  AS    $(TARGET_DIR)$@")

%.o: %.asm
	$(call quiet-command,$(ASM) $(QEMU_ASMFLAGS) -o $@ $<,"  ASM  $(TARGET_DIR)$@")

LINK = $(call quiet-command,$(CXX) $(QEMU_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(sort $(1)) $(LIBS),"  LINK  $(TARGET_DIR)$@")

%$(EXESUF): %.o
	$(call LINK,$^)

%.a:
	$(call quiet-command,rm -f $@ && $(AR) rcs $@ $^,"  AR    $(TARGET_DIR)$@")

%.bc: %.c $(GENERATED_HEADERS)
	$(call quiet-command,$(LLVMCC) $(filter-out -g -Wold-style-declaration, $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS)) -c -DS2E_LLVM_LIB -emit-llvm -o $@ $<,"  LLVMCC    $(TARGET_DIR)$@")

%.bca:
	$(call quiet-command,rm -f $@ && $(LLVMAR) rcs $@ $^,"  LLVMAR    $(TARGET_DIR)$@")

quiet-command = $(if $(V),$1,$(if $(2),@echo $2 && $1, @$1))

# cc-option
# Usage: CFLAGS+=$(call cc-option, -falign-functions=0, -malign-functions=0)

cc-option = $(if $(shell $(CC) $1 $2 -S -o /dev/null -xc /dev/null \
              >/dev/null 2>&1 && echo OK), $2, $3)

VPATH_SUFFIXES = %.c %.cpp %.h %.asm %.S %.m %.mak %.texi %.sh

set-vpath = $(if $1,$(foreach PATTERN,$(VPATH_SUFFIXES),$(eval vpath $(PATTERN) $1)))

# find-in-path
# Usage: $(call find-in-path, prog)
# Looks in the PATH if the argument contains no slash, else only considers one
# specific directory.  Returns an # empty string if the program doesn't exist
# there.
find-in-path = $(if $(find-string /, $1), \
        $(wildcard $1), \
        $(wildcard $(patsubst %, %/$1, $(subst :, ,$(PATH)))))

# Generate timestamp files for .h include files

%.h: %.h-timestamp
	@test -f $@ || cp $< $@

%.h-timestamp: %.mak
	$(call quiet-command, sh $(SRC_PATH)/scripts/create_config < $< > $@, "  GEN   $*.h")
	@cmp $@ $*.h >/dev/null 2>&1 || cp $@ $*.h

# will delete the target of a rule if commands exit with a nonzero exit status
.DELETE_ON_ERROR:
