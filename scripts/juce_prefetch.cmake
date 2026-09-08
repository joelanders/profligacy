# JUCE's pinned VST3 wrapper identifies only kOffline as non-realtime. VST3
# kPrefetch is also allowed to run faster than wall time, and must use the
# engine's blocking render path. Compile a generated wrapper with that narrow
# adaptation; leave the pinned JUCE source checkout untouched.
set(_prophecy_vst3_client "${JUCE_DIR}/modules/juce_audio_plugin_client")
file(READ "${_prophecy_vst3_client}/juce_audio_plugin_client_VST3.cpp" _prophecy_vst3_text)
foreach(_mode "newSetup.processMode" "data.processMode")
	set(_old "setNonRealtime (${_mode} == Vst::kOffline)")
	string(FIND "${_prophecy_vst3_text}" "${_old}" _found)
	if(_found LESS 0)
		message(FATAL_ERROR "Pinned JUCE VST3 process-mode contract changed; review prefetch adaptation")
	endif()
	string(REPLACE "${_old}" "setNonRealtime (${_mode} != Vst::kRealtime)"
		_prophecy_vst3_text "${_prophecy_vst3_text}")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/juce-prefetch")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/juce-prefetch/juce_audio_plugin_client_VST3.cpp" "${_prophecy_vst3_text}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/juce-prefetch/juce_audio_plugin_client_VST3.mm"
	"#include \"juce_audio_plugin_client_VST3.cpp\"\n")
get_target_property(_prophecy_vst3_sources juce::juce_audio_plugin_client_VST3 INTERFACE_SOURCES)
set(_prophecy_original_vst3_sources "${_prophecy_vst3_sources}")
string(REPLACE "${_prophecy_vst3_client}/juce_audio_plugin_client_VST3."
	"${CMAKE_CURRENT_BINARY_DIR}/juce-prefetch/juce_audio_plugin_client_VST3."
	_prophecy_vst3_sources "${_prophecy_vst3_sources}")
if(_prophecy_vst3_sources STREQUAL _prophecy_original_vst3_sources)
	message(FATAL_ERROR "Cannot select the adapted JUCE VST3 wrapper")
endif()
set_property(TARGET juce_audio_plugin_client_VST3 PROPERTY INTERFACE_SOURCES "${_prophecy_vst3_sources}")
