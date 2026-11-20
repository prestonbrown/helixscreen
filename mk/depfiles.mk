# SPDX-License-Identifier: GPL-3.0-or-later
# Reading the compiler's .d dependency files.
#
# Every .d include goes through $(call depfiles,<glob>) or
# $(call depfiles_under,<dir>), so the rules below hold for all of them.

# Dependency generation flags for proper header tracking
# -MMD: Generate .d dependency files for user headers (not system headers)
# -MP: Add phony targets for headers (prevents errors when headers are deleted)
# Note: -MF path is computed in the pattern rules to get the correct output path
DEPFLAGS = -MMD -MP

# A compile rewrites its .d in place, and a make that parses one mid-write stops
# with "missing separator". The goals below compile nothing, so they read no .d:
# land-gate runs them while its other lanes compile in this tree.
DEPFILE_FREE_GOALS := check-theme-tokens regen-tokens check-xml-schema lint-xml lint-xml-all \
                      check-lvgl-event-codes test-python
READ_DEPFILES := $(filter-out $(DEPFILE_FREE_GOALS),$(or $(MAKECMDGOALS),all))

# -MP makes every header a phony target but never the main source, so a .d
# whose source has since moved or been deleted names a prerequisite nothing can
# make. Where the object keeps its name (the display library names its objects
# by basename; foo.cpp becoming foo.c keeps obj/foo.o), that stops the build
# with "No rule to make target". Such a .d is deleted along with its object:
# the object was compiled from a file that is no longer there, so a moved
# source compiles afresh, and an object whose source list entry is gone finds
# no rule and still fails loudly.
#
# The scan reads each file only up to its first prerequisite, about 10ms per
# thousand .d files. A source name with nothing after it may be a .d still
# being written, so it is never taken as stale.
STALE_DEPFILE_SCAN := awk '{ for (i = 1; i <= NF; i++) { w = $$i; \
    if (w == "\\" || w ~ /:$$/) continue; \
    if ((i < NF || (getline more) > 0) && (getline junk < w) < 0) print FILENAME; \
    else close(w); nextfile } }'

# depfiles <glob>: the .d files the glob matches, less the stale ones.
# depfiles_under <dir>: the same for every .d below <dir>.
# The shell expands the glob, or find lists the files, rather than make passing
# them in: one shell command holding a few thousand paths is longer than the
# kernel's 128KB limit on a single argument.
depfiles = $(if $(READ_DEPFILES),$(call drop_stale_depfiles,$(wildcard $(1)),$(if $(wildcard $(1)),$(shell $(STALE_DEPFILE_SCAN) $(1) </dev/null 2>/dev/null))))
depfiles_under = $(if $(READ_DEPFILES),$(call drop_stale_depfiles,$(shell find $(1) -name '*.d' 2>/dev/null),$(shell find $(1) -name '*.d' -exec $(STALE_DEPFILE_SCAN) {} + 2>/dev/null)))
drop_stale_depfiles = $(if $(2),$(shell rm -f $(2) $(2:.d=.o)))$(filter-out $(2),$(1))
