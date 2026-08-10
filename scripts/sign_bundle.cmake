# Sign a plugin bundle with hardened runtime + the JIT entitlement, and re-sign the
# installed copy if JUCE's copy-after-build step already placed one (that copy runs
# before this signing step, so it would otherwise keep the unsigned/stale signature).
#
# usage: cmake -Didentity=<id> -Dentitlements=<plist> -Dbundle=<dir> [-Dinstalled=<dir>] -P sign_bundle.cmake

execute_process(
	COMMAND codesign --force --sign "${identity}" --options runtime
		--entitlements "${entitlements}" "${bundle}"
	COMMAND_ERROR_IS_FATAL ANY)

if(installed AND EXISTS "${installed}")
	execute_process(
		COMMAND codesign --force --sign "${identity}" --options runtime
			--entitlements "${entitlements}" "${installed}"
		COMMAND_ERROR_IS_FATAL ANY)
endif()
