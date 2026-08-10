# Staleness guard for the linked MAME archives (run at configure time from
# CMakeLists.txt and at build time as the check_mame_fresh target).
#
# Inputs: -DMAME_DIR=... -DMAME_BIN=...
# Passes when ${MAME_BIN}/.prophecy_build_rev (written by scripts/build_mame_core.sh)
# matches the submodule's current HEAD. A "-dirty" stamp or a missing stamp falls
# back to a warning + mtime heuristic (archive must not predate the HEAD commit).

if(NOT DEFINED MAME_DIR OR NOT DEFINED MAME_BIN)
	message(FATAL_ERROR "check_mame_fresh.cmake needs -DMAME_DIR and -DMAME_BIN")
endif()

set(_stamp_file "${MAME_BIN}/.prophecy_build_rev")
set(_rebuild_hint "  Rebuild + restamp: scripts/build_mame_core.sh")

execute_process(
	COMMAND git -C "${MAME_DIR}" rev-parse HEAD
	OUTPUT_VARIABLE _head_rev OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE _git_rc)
if(NOT _git_rc EQUAL 0)
	message(WARNING "check_mame_fresh: cannot read extern/mame HEAD (not a git checkout?) — skipping staleness check")
	return()
endif()

if(EXISTS "${_stamp_file}")
	file(READ "${_stamp_file}" _stamp)
	string(STRIP "${_stamp}" _stamp)
	if(_stamp STREQUAL "${_head_rev}")
		return() # fresh
	endif()
	if(_stamp MATCHES "-dirty$")
		message(WARNING "check_mame_fresh: archives were built from a DIRTY tree (${_stamp}); current HEAD is ${_head_rev}. Link proceeds, but do not ship this build.")
		return()
	endif()
	message(FATAL_ERROR
		"MAME archives are STALE: built from ${_stamp}\n"
		"but extern/mame HEAD is           ${_head_rev}\n"
		${_rebuild_hint})
endif()

# No stamp (archives predate the stamp mechanism): mtime heuristic — the newest
# commit must not be newer than the archive that links the driver.
execute_process(
	COMMAND git -C "${MAME_DIR}" log -1 --format=%ct HEAD
	OUTPUT_VARIABLE _head_time OUTPUT_STRIP_TRAILING_WHITESPACE)
if(WIN32)
	set(_driver_archive "${MAME_BIN}/mame_propmin/mame_propmin.lib")
else()
	set(_driver_archive "${MAME_BIN}/mame_propmin/libmame_propmin.a")
endif()
file(TIMESTAMP "${_driver_archive}" _arch_time "%s")
if(_arch_time AND _head_time AND _arch_time LESS _head_time)
	message(FATAL_ERROR
		"MAME archives predate the extern/mame HEAD commit (no build stamp found; "
		"archive mtime ${_arch_time} < commit time ${_head_time}).\n"
		${_rebuild_hint})
endif()
message(WARNING "check_mame_fresh: no build stamp at ${_stamp_file} (pre-stamp archives); mtime heuristic passed. Rebuild with scripts/build_mame_core.sh to get exact rev checking.")
