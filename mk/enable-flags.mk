# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every ENABLE_* build flag is a yes/no switch, and the makefiles test it as
# `ifeq ($(ENABLE_X),yes)`. A value given on the command line is normalized
# here, before any of those tests run:
#
#   1 yes true on   -> yes
#   0 no false off  -> no      (an empty value is no as well)
#
# in any letter case. Anything else stops the build, so a typo cannot silently
# leave a feature off. Values from the environment are left as they are: a
# makefile assignment outranks the environment, and rewriting one here would
# change which wins.

HELIX_FLAG_YES := 1 yes true on
HELIX_FLAG_NO := 0 no false off

# $(call helix_flag_value,NAME): NAME's value as yes or no, or an error naming it.
helix_flag_value = $(strip $(if $(word 2,$($(1))),$(call helix_flag_bad,$(1)),$(if \
    $(strip $($(1))),$(call helix_flag_word,$(1),$(call helix_flag_lower,$(strip $($(1))))),no)))
helix_flag_word = $(if $(filter $(2),$(HELIX_FLAG_YES)),yes,$(if \
    $(filter $(2),$(HELIX_FLAG_NO)),no,$(call helix_flag_bad,$(1))))
helix_flag_lower = $(shell printf '%s' '$(1)' | tr '[:upper:]' '[:lower:]')
helix_flag_bad = $(error $(1)=$($(1)) is not a yes/no value: use one of $(HELIX_FLAG_YES) $(HELIX_FLAG_NO))

$(foreach flag,$(filter ENABLE_%,$(.VARIABLES)),$(if \
    $(filter command line,$(origin $(flag))),$(eval override $(flag) := $(call helix_flag_value,$(flag)))))
