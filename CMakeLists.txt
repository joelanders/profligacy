cmake_minimum_required(VERSION 3.22)
project(profligacy_plugin VERSION 1.0.0 LANGUAGES C CXX)

option(PROPHECY_USE_FAKE_ENGINE
	"Build the JUCE shell against a deterministic no-MAME engine stub"
	OFF)

if(APPLE)
	enable_language(OBJC OBJCXX)
endif()

include(CTest)

if(BUILD_TESTING)
	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	foreach(_prophecy_python_test
		alpha_release_policy
		build_ci_probe_roms
		correctness_checkpoint
		editor_asset_provenance
		generate_au_link_license_ledger
		grade_arp_hardware_capture
		mame_sdk
		product_identity
		promote_arm64_dynarec_receipt
		release_preflight
		release_licensing
		run_ci_rom_e2e
		run_arp_timing_suite
		run_control_abba_repeatability
		run_clean_mame_performance_equivalence
		run_threaded_pf4_cmem_equivalence)
		add_test(
			NAME ${_prophecy_python_test}
			COMMAND ${Python3_EXECUTABLE} -B
				${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_${_prophecy_python_test}.py)
	endforeach()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
	# The Windows ClangCL MAME archives use the static Microsoft CRT (/MT). Keep JUCE and
	# the host on the same runtime to avoid both unresolved __imp_* CRT symbols
	# and duplicate CRT definitions at the final link.
	if(PROPHECY_USE_FAKE_ENGINE)
		# The fake shell has no MAME archives and follows JUCE's normal /MD choice.
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
	else()
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
	endif()
	# JUCE's generated module projects can append their own /MD flag after the
	# target property. Put /MT at the end of the Release compiler flags as the
	# generator-independent fallback for the MAME-compatible shell build.
	if((MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC") AND NOT PROPHECY_USE_FAKE_ENGINE)
		string(APPEND CMAKE_C_FLAGS_RELEASE " /MT")
		string(APPEND CMAKE_CXX_FLAGS_RELEASE " /MT")
	endif()
endif()
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
	set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)   # match the MAME arm64 archives
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
	# Must match the MAME archives' min-OS (they are built for 15.0; a lower
	# plugin target just produces linker warnings and a binary that still
	# requires 15.0 at runtime — say so honestly).
	set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# MAME: the fork submodule, built once (see README). We only link its archives.
# ---------------------------------------------------------------------------
if(NOT PROPHECY_USE_FAKE_ENGINE)
set(MAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/mame" CACHE PATH "MAME fork tree (must be built)")
if(WIN32)
	set(MAME_BIN "${MAME_DIR}/build/vs2022/bin/x64/Release")
	set(MAME_OBJ "${MAME_DIR}/build/vs2022/obj/x64/Release")
	set(MAME_LIB_SUFFIX ".lib")
	set(MAME_OSD_LIBS libocore_windows.lib libosd_windows.lib libqtdbg_windows.lib)
else()
	set(MAME_BIN "${MAME_DIR}/build/osx_clang/bin/x64/Release")
	set(MAME_OBJ "${MAME_DIR}/build/osx_clang/obj/x64/Release")
	set(MAME_LIB_SUFFIX ".a")
	set(MAME_OSD_LIBS libosd_sdl.a libqtdbg_sdl.a libocore_sdl.a)
endif()
if(WIN32)
	set(_mame_probe_archive "${MAME_BIN}/emu.lib")
else()
	set(_mame_probe_archive "${MAME_BIN}/libemu.a")
endif()
if(NOT EXISTS "${_mame_probe_archive}")
	message(FATAL_ERROR "MAME archives not found at ${MAME_BIN}\n  Build the submodule once: scripts/build_mame_core.sh --regen")
endif()
# Refuse to link archives built from a different extern/mame rev than the one
# checked out (the stamp is written by scripts/build_mame_core.sh). Checked at
# configure time here, and again at build time below so a submodule ff after
# configure still errors instead of silently linking stale code.
execute_process(COMMAND ${CMAKE_COMMAND}
	-DMAME_DIR=${MAME_DIR} -DMAME_BIN=${MAME_BIN}
	-P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mame_fresh.cmake
	RESULT_VARIABLE _mame_fresh_rc)
if(NOT _mame_fresh_rc EQUAL 0)
	message(FATAL_ERROR "extern/mame archive staleness check failed (see above)")
endif()
execute_process(COMMAND ${CMAKE_COMMAND}
	-DMAME_BIN=${MAME_BIN}
	-P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mame_host_abi.cmake
	RESULT_VARIABLE _mame_host_abi_rc)
if(NOT _mame_host_abi_rc EQUAL 0)
	message(FATAL_ERROR "extern/mame host ABI check failed (see above)")
endif()
add_custom_target(check_mame_fresh
	COMMAND ${CMAKE_COMMAND}
		-DMAME_DIR=${MAME_DIR} -DMAME_BIN=${MAME_BIN}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mame_fresh.cmake
	COMMENT "checking MAME archives match extern/mame HEAD"
	VERBATIM)
add_custom_target(check_mame_host_abi
	COMMAND ${CMAKE_COMMAND}
		-DMAME_BIN=${MAME_BIN}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mame_host_abi.cmake
	COMMENT "checking MAME archives export the prophecy-plugin host ABI"
	VERBATIM)

if(NOT WIN32)
	set(SDL_PREFIX "/opt/homebrew" CACHE PATH "SDL2 static install prefix")
endif()
endif()

# ---------------------------------------------------------------------------
# prophecy_engine: the ONLY target compiled against MAME headers. Its public
# interface is the MAME-free prophecy_engine.h; MAME archives link through it.
# ---------------------------------------------------------------------------
if(PROPHECY_USE_FAKE_ENGINE)
	set(_prophecy_engine_source src/prophecy_engine_fake.cpp)
else()
	set(_prophecy_engine_source src/prophecy_engine.cpp)
endif()
add_library(prophecy_engine STATIC ${_prophecy_engine_source})
if(NOT PROPHECY_USE_FAKE_ENGINE)
target_compile_features(prophecy_engine PRIVATE cxx_std_20)
add_dependencies(prophecy_engine check_mame_fresh check_mame_host_abi)

target_include_directories(prophecy_engine
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
	PRIVATE
		${MAME_DIR}/src/osd ${MAME_DIR}/src/emu ${MAME_DIR}/src/devices ${MAME_DIR}/src/mame
		${MAME_DIR}/src/lib ${MAME_DIR}/src/lib/util ${MAME_DIR}/3rdparty
		${MAME_DIR}/build/generated/mame/layout ${MAME_DIR}/3rdparty/zlib ${MAME_DIR}/3rdparty/flac/include
		${MAME_DIR}/src/osd/modules ${MAME_DIR}/src/osd/modules/file)
if(WIN32)
	target_include_directories(prophecy_engine PRIVATE ${MAME_DIR}/src/osd/windows)
else()
	target_include_directories(prophecy_engine PRIVATE
		${MAME_DIR}/src/osd/sdl ${SDL_PREFIX}/include ${SDL_PREFIX}/include/SDL2)
endif()

target_compile_definitions(prophecy_engine PRIVATE NDEBUG CRLF=2 LSB_FIRST FLAC__NO_DLL
	PUGIXML_HEADER_ONLY ASMJIT_STATIC LUA_COMPAT_ALL LUA_COMPAT_5_1 LUA_COMPAT_5_2
	USE_NETWORK OSD_NET_USE_PCAP)
if(WIN32)
	target_compile_definitions(prophecy_engine PRIVATE SDLMAME_WIN32 OSD_WINDOWS)
else()
	target_compile_definitions(prophecy_engine PRIVATE SDLMAME_NO_X11 USE_XINPUT=0
		USE_XINPUT_WII_LIGHTGUN_HACK=0 SDLMAME_SDL2=1 OSD_SDL SDLMAME_UNIX
		MACOSX_USE_LIBSDL SDLMAME_MACOSX SDLMAME_DARWIN _THREAD_SAFE)
endif()

if(NOT MSVC)
	target_compile_options(prophecy_engine PRIVATE
		-Wno-unknown-pragmas -Wno-unknown-warning-option -Wno-conversion -Wno-sign-compare
		-Wno-deprecated-declarations -Wno-unused-value -Wno-tautological-compare
		-Wno-ignored-reference-qualifiers)
endif()

# MAME static archives, in dependency order (from the propmin link line).
if(WIN32)
	set(MAME_ARCHIVES
		${MAME_BIN}/mame_propmin/mame_propmin.lib ${MAME_BIN}/frontend.lib ${MAME_BIN}/mame_propmin/optional.lib
		${MAME_BIN}/emu.lib ${MAME_BIN}/osd_windows.lib ${MAME_BIN}/qtdbg_windows.lib ${MAME_BIN}/mame_propmin/formats.lib
		${MAME_BIN}/mame_propmin/dasm.lib ${MAME_BIN}/utils.lib ${MAME_BIN}/expat.lib ${MAME_BIN}/softfloat3.lib
		${MAME_BIN}/wdlfft.lib ${MAME_BIN}/ymfm.lib ${MAME_BIN}/jpeg.lib ${MAME_BIN}/7z.lib
		${MAME_BIN}/asmjit.lib ${MAME_BIN}/lua.lib ${MAME_BIN}/lualibs.lib ${MAME_BIN}/linenoise.lib ${MAME_BIN}/zlib.lib
		${MAME_BIN}/zstd.lib ${MAME_BIN}/flac.lib ${MAME_BIN}/utf8proc.lib ${MAME_BIN}/sqlite3.lib ${MAME_BIN}/bgfx.lib
		${MAME_BIN}/bimg.lib ${MAME_BIN}/bx.lib ${MAME_BIN}/portaudio.lib ${MAME_BIN}/portmidi.lib
		${MAME_BIN}/ocore_windows.lib)
else()
	set(MAME_ARCHIVES
		${MAME_BIN}/mame_propmin/libmame_propmin.a ${MAME_BIN}/libfrontend.a ${MAME_BIN}/mame_propmin/liboptional.a
		${MAME_BIN}/libemu.a ${MAME_BIN}/libosd_sdl.a ${MAME_BIN}/libqtdbg_sdl.a ${MAME_BIN}/mame_propmin/libformats.a
		${MAME_BIN}/mame_propmin/libdasm.a ${MAME_BIN}/libutils.a ${MAME_BIN}/libexpat.a ${MAME_BIN}/libsoftfloat3.a
		${MAME_BIN}/libwdlfft.a ${MAME_BIN}/libymfm.a ${MAME_BIN}/libjpeg.a ${MAME_BIN}/lib7z.a
		${MAME_BIN}/libasmjit.a ${MAME_BIN}/liblua.a ${MAME_BIN}/liblualibs.a ${MAME_BIN}/liblinenoise.a ${MAME_BIN}/libzlib.a
		${MAME_BIN}/libzstd.a ${MAME_BIN}/libflac.a ${MAME_BIN}/libutf8proc.a ${MAME_BIN}/libsqlite3.a ${MAME_BIN}/libbgfx.a
		${MAME_BIN}/libbimg.a ${MAME_BIN}/libbx.a ${MAME_BIN}/libocore_sdl.a)
endif()

if(WIN32)
	set(MAME_OBJECTS
		${MAME_OBJ}/propmin/mame.obj ${MAME_OBJ}/propmin/drivlist.obj ${MAME_OBJ}/propmin/version.obj)
else()
	set(MAME_OBJECTS
		${MAME_OBJ}/src/mame/mame.o ${MAME_OBJ}/generated/mame/propmin/drivlist.o
		${MAME_OBJ}/generated/version.o)
endif()
set(_missing_mame_link_inputs)
foreach(_mame_link_input IN LISTS MAME_OBJECTS MAME_ARCHIVES)
	if(NOT EXISTS "${_mame_link_input}")
		list(APPEND _missing_mame_link_inputs "${_mame_link_input}")
	endif()
endforeach()
if(_missing_mame_link_inputs)
	list(JOIN _missing_mame_link_inputs "\n  " _missing_mame_link_inputs_text)
	message(FATAL_ERROR "required MAME link inputs are missing:\n  ${_missing_mame_link_inputs_text}")
endif()

if(WIN32)
	target_link_libraries(prophecy_engine PUBLIC ${MAME_OBJECTS} ${MAME_ARCHIVES}
		user32 advapi32 wsock32 ws2_32 iphlpapi shell32 userenv setupapi opengl32
		gdi32 dsound dxguid oleaut32 winmm bcrypt comctl32 comdlg32 dinput8 ole32
		propsys psapi shcore shlwapi uuid kernel32 winspool odbc32 odbccp32)
else()
	target_link_libraries(prophecy_engine PUBLIC
		${MAME_OBJECTS} ${MAME_ARCHIVES}
		${SDL_PREFIX}/lib/libSDL2.a pthread m objc
		"-framework QuartzCore" "-framework OpenGL" "-framework IOKit" "-weak_framework Metal"
		"-framework CoreAudio" "-framework AudioToolbox" "-weak_framework CoreHaptics"
		"-weak_framework GameController" "-framework ForceFeedback" "-framework CoreVideo"
		"-framework Cocoa" "-framework Carbon" "-framework AudioUnit" "-framework CoreServices")
endif()
endif()

# Non-shipping acceptance host: construct, boot, stop, destroy, and reconstruct
# ProphecyEngine repeatedly in one process.  This catches stale MAME singleton,
# callback, and worker-thread state that one-process-per-test scripts cannot see.
add_executable(ProphecyLifecycleHost EXCLUDE_FROM_ALL src/lifecycle_main.cpp)
target_link_libraries(ProphecyLifecycleHost PRIVATE prophecy_engine)

# Fast no-ROM statement of the v1 instance contract. This distinguishes "a host can
# construct/tear down two plugin objects" from independent simultaneous synthesis.
add_executable(ProphecyMultiInstanceContractTest scripts/multi_instance_contract_test.cpp)
target_link_libraries(ProphecyMultiInstanceContractTest PRIVATE prophecy_engine)
if(WIN32 AND NOT PROPHECY_USE_FAKE_ENGINE)
	# MAME's Windows OSD expects the Common Controls v6 activation context from
	# its executable resource.  Add that dependency to MSBuild's generated
	# executable manifest (JUCE executables embed an equivalent dependency).
	target_link_options(ProphecyMultiInstanceContractTest PRIVATE
		"/MANIFESTDEPENDENCY:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
endif()
if(BUILD_TESTING)
	add_test(NAME multi_instance_contract COMMAND $<TARGET_FILE:ProphecyMultiInstanceContractTest>)
	set_tests_properties(multi_instance_contract PROPERTIES TIMEOUT 15)
endif()

# ---------------------------------------------------------------------------
# JUCE plugin
# ---------------------------------------------------------------------------
add_subdirectory(extern/JUCE)

if(WIN32 AND NOT PROPHECY_USE_FAKE_ENGINE)
	# JUCE module targets do not consistently inherit the directory-level
	# runtime setting when generated by the Visual Studio multi-config
	# generator. Align the modules explicitly with the MAME/host /MT choice.
	foreach(_prophecy_juce_target
		juce_core juce_data_structures juce_events juce_graphics juce_gui_basics
		juce_gui_extra juce_audio_basics juce_audio_devices juce_audio_formats
		juce_audio_processors juce_audio_utils juce_dsp)
		if(TARGET ${_prophecy_juce_target})
			set_property(TARGET ${_prophecy_juce_target} PROPERTY
				MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		endif()
	endforeach()
endif()

option(PROPHECY_COPY_PLUGIN_AFTER_BUILD
	"copy AU/VST3 bundles into the current user's plug-in directories"
	ON)

if(WIN32)
	set(PROPHECY_PLUGIN_FORMATS VST3 Standalone)
	set(PROPHECY_WEBVIEW2_TARGET_ARGS NEEDS_WEBVIEW2 TRUE)
else()
	set(PROPHECY_PLUGIN_FORMATS AU VST3 Standalone)
	set(PROPHECY_WEBVIEW2_TARGET_ARGS)
endif()

juce_add_plugin(ProphecyPlugin
	PRODUCT_NAME "Profligacy"
	COMPANY_NAME "Profligacy"
	BUNDLE_ID "net.joelanders.profligacy"
	PLUGIN_MANUFACTURER_CODE Prfl
	PLUGIN_CODE Pflg
	FORMATS ${PROPHECY_PLUGIN_FORMATS}
	IS_SYNTH TRUE
	NEEDS_MIDI_INPUT TRUE
	EDITOR_WANTS_KEYBOARD_FOCUS FALSE
	${PROPHECY_WEBVIEW2_TARGET_ARGS}
	COPY_PLUGIN_AFTER_BUILD ${PROPHECY_COPY_PLUGIN_AFTER_BUILD})

if(WIN32 AND NOT PROPHECY_USE_FAKE_ENGINE)
	# juce_add_plugin may instantiate module targets lazily, so repeat the
	# runtime alignment after the plugin has created its module dependencies.
	foreach(_prophecy_juce_target
		juce_core juce_data_structures juce_events juce_graphics juce_gui_basics
		juce_gui_extra juce_audio_basics juce_audio_devices juce_audio_formats
		juce_audio_processors juce_audio_utils juce_dsp)
		if(TARGET ${_prophecy_juce_target})
			set_property(TARGET ${_prophecy_juce_target} PROPERTY
				MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		endif()
	endforeach()
endif()

# Opt-in, exact-link provenance artifact for release audits.  Normal builds do
# not add a linker diagnostic or create a map.  The map belongs to the AU
# wrapper target; ProphecyPlugin shared-code and prophecy_engine objects pulled
# into that wrapper are therefore represented by what ld actually retained.
set(PROPHECY_AU_LINK_MAP "" CACHE FILEPATH
	"write an Apple ld map for ProphecyPlugin_AU (disabled when empty)")
if(PROPHECY_AU_LINK_MAP)
	get_filename_component(_prophecy_au_link_map
		"${PROPHECY_AU_LINK_MAP}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
	get_filename_component(_prophecy_au_link_map_dir "${_prophecy_au_link_map}" DIRECTORY)
	file(MAKE_DIRECTORY "${_prophecy_au_link_map_dir}")
	target_link_options(ProphecyPlugin_AU PRIVATE
		"SHELL:-Wl,-map,${_prophecy_au_link_map}")
endif()

# ---------------------------------------------------------------------------
# Notarization prep: hardened-runtime signing on every bundle, plus the JIT
# entitlement where it can take effect. Semantics worth knowing (verified):
#   - Entitlements (incl. com.apple.security.cs.allow-jit) attach only to MAIN
#     EXECUTABLES. The Standalone app embeds it (checked via codesign -d
#     --entitlements). The AU/VST3 binaries are Mach-O *bundles*: codesign
#     silently omits entitlements for them, by design — a plugin's JIT runs
#     under the HOST process's entitlements (auval runs the dynarec at ~190%,
#     and JIT-carrying plugins like gearmulator ship fine in DAWs).
#   - Notarization needs the hardened-runtime flag on all signed code; that IS
#     applied to all three formats here.
# JUCE's HARDENED_RUNTIME_ENABLED only works under the Xcode generator, so for
# this Makefile build we re-sign each bundle post-build (ad-hoc by default;
# set PROPHECY_CODESIGN_IDENTITY to a "Developer ID Application: ..." identity
# for a distributable signature — notarization then needs only a submit step).
# ---------------------------------------------------------------------------
if(APPLE)
set(PROPHECY_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity (default: ad-hoc)")
set(_prophecy_entitlements ${CMAKE_CURRENT_SOURCE_DIR}/entitlements/profligacy.entitlements)
# JUCE's copy-after-build step runs before this one, so the signing script also
# re-signs the installed copy when present.
set(_prophecy_installed_AU "")
set(_prophecy_installed_VST3 "")
set(_prophecy_installed_Standalone "")
if(PROPHECY_COPY_PLUGIN_AFTER_BUILD)
	set(_prophecy_installed_AU   "$ENV{HOME}/Library/Audio/Plug-Ins/Components/Profligacy.component")
	set(_prophecy_installed_VST3 "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/Profligacy.vst3")
endif()
foreach(_fmt AU VST3 Standalone)
	if(TARGET ProphecyPlugin_${_fmt})
		# With Makefile/Ninja generators, CMake emits JUCE's automatic
		# RecentFilesMenuTemplate.nib bundle copy after target POST_BUILD
		# commands.  Put the same bytes in place before signing so the final
		# automatic copy cannot add an unsealed resource to the app.
		if(_fmt STREQUAL "Standalone")
			add_custom_command(TARGET ProphecyPlugin_${_fmt} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					${JUCE_CMAKE_UTILS_DIR}/RecentFilesMenuTemplate.nib
					$<TARGET_BUNDLE_CONTENT_DIR:ProphecyPlugin_${_fmt}>/Resources/RecentFilesMenuTemplate.nib
				COMMENT "stage final Standalone NIB before codesigning"
				VERBATIM)
		endif()
		add_custom_command(TARGET ProphecyPlugin_${_fmt} POST_BUILD
			COMMAND ${CMAKE_COMMAND}
				-Didentity=${PROPHECY_CODESIGN_IDENTITY}
				-Dentitlements=${_prophecy_entitlements}
				-Dbundle=$<TARGET_BUNDLE_DIR:ProphecyPlugin_${_fmt}>
				-Dinstalled=${_prophecy_installed_${_fmt}}
				-P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sign_bundle.cmake
			COMMENT "codesign (hardened runtime + allow-jit): ProphecyPlugin_${_fmt}"
			VERBATIM)
	endif()
endforeach()
endif()

target_sources(ProphecyPlugin PRIVATE src/PluginProcessor.cpp)

# Embed the WebView editor's HTML/CSS/JS (single self-contained file for now).
juce_add_binary_data(ProphecyData SOURCES
	src/editor/index.html
	src/editor/deep_editor_manifest.js
	src/editor/assets/NotoSans-Bold.ttf)

target_compile_definitions(ProphecyPlugin PRIVATE
	JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
if(WIN32)
	# NEEDS_WEBVIEW2 links WebView2LoaderStatic.lib. Select the matching JUCE
	# implementation; the editor options below explicitly select this backend.
	target_compile_definitions(ProphecyPlugin PRIVATE
		JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
endif()

target_link_libraries(ProphecyPlugin
	PRIVATE
		prophecy_engine
		ProphecyData
		juce::juce_audio_utils
		juce::juce_gui_extra
	PUBLIC
		juce::juce_recommended_config_flags
		juce::juce_recommended_warning_flags)

# Headless real-processor timing harness. It drives processBlock at selectable host
# rates/block sizes and captures the emulated physical MIDI OUT UART; it is not shipped.
add_executable(ProphecyPluginTimingHost scripts/plugin_timing_host.cpp)
target_include_directories(ProphecyPluginTimingHost PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/extern/JUCE/modules)
target_compile_definitions(ProphecyPluginTimingHost PRIVATE
	JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(ProphecyPluginTimingHost PRIVATE ProphecyPlugin)
if(WIN32)
	target_compile_definitions(ProphecyPluginTimingHost PRIVATE
		JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
	target_link_libraries(ProphecyPluginTimingHost PRIVATE juce::juce_webview2)
endif()

# Fast no-ROM regression test for processBlock's release-critical real-time contract:
# no allocation after prepareToPlay, bounded oversize behavior, and observable queue drops.
add_executable(ProphecyPluginRtSafetyTest scripts/plugin_rt_safety_test.cpp)
target_include_directories(ProphecyPluginRtSafetyTest PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/extern/JUCE/modules)
target_compile_definitions(ProphecyPluginRtSafetyTest PRIVATE
	JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(ProphecyPluginRtSafetyTest PRIVATE ProphecyPlugin)
if(WIN32)
	target_compile_definitions(ProphecyPluginRtSafetyTest PRIVATE
		JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
	target_link_libraries(ProphecyPluginRtSafetyTest PRIVATE juce::juce_webview2)
endif()
if(BUILD_TESTING)
	add_test(NAME plugin_rt_safety COMMAND $<TARGET_FILE:ProphecyPluginRtSafetyTest>)
	set_tests_properties(plugin_rt_safety PROPERTIES TIMEOUT 10)
endif()

# Artifact-level VST3 smoke host. Unlike the timing and RT harnesses above, this
# executable does not link ProphecyPlugin: it scans and loads the packaged VST3
# through JUCE's host API, then drives its public MIDI/audio/state interface.
juce_add_console_app(ProphecyArtifactHost PRODUCT_NAME "Prophecy Artifact Host")
target_sources(ProphecyArtifactHost PRIVATE scripts/plugin_artifact_host.cpp)
target_compile_definitions(ProphecyArtifactHost PRIVATE
	JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(ProphecyArtifactHost PRIVATE
	juce::juce_audio_utils juce::juce_audio_formats)

if(WIN32 AND NOT PROPHECY_USE_FAKE_ENGINE)
	foreach(_prophecy_windows_target prophecy_engine ProphecyPlugin ProphecyMultiInstanceContractTest ProphecyPluginTimingHost ProphecyPluginRtSafetyTest ProphecyArtifactHost)
		if(TARGET ${_prophecy_windows_target})
			set_property(TARGET ${_prophecy_windows_target} PROPERTY
				MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		endif()
	endforeach()
endif()
