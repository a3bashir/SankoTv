# Writes the CURRENT git HEAD into a generated header, run on EVERY build.
#
# The hash used to be captured with execute_process at CONFIGURE time and
# baked in as a compile definition, so it only changed when CMake happened to
# reconfigure. A recorded session proved the cost: system.txt named a commit
# four commits behind what was actually running, which is provenance that
# quietly lies — worse than none, because it is believed.
#
# The header is rewritten ONLY when the value changes, so a build whose HEAD
# has not moved does not recompile anything.
execute_process(COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE GIT_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GIT_HEAD)
    set(GIT_HEAD "unknown")
endif()

# A build from a dirty tree is not any commit, and the hash alone cannot say
# so. Mark it, so a session recorded from uncommitted work is legible as
# such rather than being blamed on the commit it was nearest to.
execute_process(COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(GIT_DIRTY)
    set(GIT_HEAD "${GIT_HEAD}+dirty")
endif()

set(CONTENT "#pragma once\n// GENERATED at build time - do not edit.\n")
string(APPEND CONTENT "#define SANKOTV_GIT_HEAD \"${GIT_HEAD}\"\n")

set(EXISTING "")
if(EXISTS ${OUT})
    file(READ ${OUT} EXISTING)
endif()
if(NOT EXISTING STREQUAL CONTENT)
    file(WRITE ${OUT} "${CONTENT}")
endif()
