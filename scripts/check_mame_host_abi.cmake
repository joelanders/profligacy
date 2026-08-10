# Fail before compilation/linking if the selected Prophecy MAME archive does
# not export the complete statically-linked host interface used by the plugin.

if(NOT DEFINED MAME_BIN)
	message(FATAL_ERROR "check_mame_host_abi.cmake needs -DMAME_BIN")
endif()

if(WIN32)
	set(_driver_archive "${MAME_BIN}/mame_propmin/mame_propmin.lib")
else()
	set(_driver_archive "${MAME_BIN}/mame_propmin/libmame_propmin.a")
endif()
if(NOT EXISTS "${_driver_archive}")
	message(FATAL_ERROR "MAME driver archive not found: ${_driver_archive}")
endif()

if(WIN32)
	find_program(_dumpbin NAMES dumpbin dumpbin.exe)
	if(NOT _dumpbin)
		message(WARNING "check_mame_host_abi: dumpbin not found; skipping Windows archive symbol check")
		return()
	endif()
	execute_process(
		COMMAND "${_dumpbin}" /symbols "${_driver_archive}"
		OUTPUT_VARIABLE _global_symbols
		ERROR_VARIABLE _nm_error
		RESULT_VARIABLE _nm_rc)
else()
	execute_process(
		COMMAND nm -gU "${_driver_archive}"
		OUTPUT_VARIABLE _global_symbols
		ERROR_VARIABLE _nm_error
		RESULT_VARIABLE _nm_rc)
endif()
if(NOT _nm_rc EQUAL 0)
	message(FATAL_ERROR "cannot inspect MAME host ABI in ${_driver_archive}: ${_nm_error}")
endif()

set(_required_symbols
	kprop_set_host_midi_pop
	kprop_set_host_midi_tx
	kprop_set_host_midi_tx_byte
	kprop_set_host_lcd
	kprop_set_host_lcd_raw
	kprop_set_host_panel_pop
	kprop_set_host_adin_pop
	kprop_set_host_led)

set(_missing_symbols "")
foreach(_symbol IN LISTS _required_symbols)
	if(NOT _global_symbols MATCHES "[ \t]_?${_symbol}([\r\n]|$)")
		list(APPEND _missing_symbols "${_symbol}")
	endif()
endforeach()

if(_missing_symbols)
	list(JOIN _missing_symbols "\n  " _missing_lines)
	message(FATAL_ERROR
		"MAME archive is missing prophecy-plugin host ABI symbols:\n"
		"  ${_missing_lines}\n"
		"Archive: ${_driver_archive}\n"
		"Rebuild the selected MAME tree before linking the plugin.")
endif()
