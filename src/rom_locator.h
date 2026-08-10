// SPDX-License-Identifier: AGPL-3.0-only
//
// rom_locator.h - find (or remember) the user's Prophecy ROM set. Host-side only,
// no MAME includes. The plugin cannot ship Korg's firmware, so the user supplies a
// folder laid out the way MAME's -rompath expects; we validate, persist their choice,
// and pick a writable NVRAM home.
//
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdlib>

namespace romloc {

// A valid Prophecy ROM directory, as MAME's -rompath sees it.  The plug-in's
// explicit LCD device contains the published A00 datasheet reconstruction, so
// the user supplies only the two Korg firmware images (or korgprop.zip).
inline bool isValidRomDir(const juce::File &dir)
{
	if (!dir.isDirectory())
		return false;
	const bool korgprop =
		(dir.getChildFile("korgprop").getChildFile("ic12_v17.bin").existsAsFile() &&
		 dir.getChildFile("korgprop").getChildFile("ic22_v17.bin").existsAsFile()) ||
		dir.getChildFile("korgprop.zip").existsAsFile();
	return korgprop;
}

inline juce::PropertiesFile::Options settingsOptions()
{
	juce::PropertiesFile::Options o;
	o.applicationName     = "Profligacy";
	o.filenameSuffix      = ".settings";
	o.folderName          = "Profligacy";
	o.osxLibrarySubFolder = "Application Support";
	return o;
}

inline juce::PropertiesFile::Options legacySettingsOptions()
{
	juce::PropertiesFile::Options o;
	o.applicationName     = "Prophecy";
	o.filenameSuffix      = ".settings";
	o.folderName          = "Prophecy";
	o.osxLibrarySubFolder = "Application Support";
	return o;
}

inline juce::File appSupportDir()
{
	// On macOS userApplicationDataDirectory is ~/Library (NOT Application Support) —
	// build the conventional path explicitly so files land where users expect.
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
#if JUCE_MAC
		.getChildFile("Application Support")
#endif
		.getChildFile("Profligacy");
}

inline juce::File legacyAppSupportDir()
{
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
#if JUCE_MAC
		.getChildFile("Application Support")
#endif
		.getChildFile("Prophecy");
}

inline void persistRomDir(const juce::File &dir);

// Search order: env override -> the user's persisted picker choice -> the documented
// drop location (~/Library/Application Support/Profligacy/roms), then the legacy
// Prophecy settings/drop location. Returns an invalid File() when nothing validates.
inline juce::File locateRomDir()
{
	// Test hook: force the no-ROM state on a machine that DOES have ROMs, so the
	// first-run picker overlay + error paths can be exercised headlessly. Off unless set.
	if (std::getenv("PROPHECY_FORCE_NO_ROM"))
		return {};
	if (const char *env = std::getenv("PROPHECY_ROMPATH"); env && *env)
	{
		const juce::File f{juce::String::fromUTF8(env)};
		if (isValidRomDir(f))
			return f;
	}
	{
		juce::PropertiesFile props(settingsOptions());
		const juce::String saved = props.getValue("romPath");
		if (saved.isNotEmpty() && isValidRomDir(juce::File(saved)))
			return juce::File(saved);
	}
	if (const juce::File drop = appSupportDir().getChildFile("roms"); isValidRomDir(drop))
		return drop;
	{
		juce::PropertiesFile legacy(legacySettingsOptions());
		const juce::String saved = legacy.getValue("romPath");
		if (saved.isNotEmpty() && isValidRomDir(juce::File(saved)))
		{
			persistRomDir(juce::File(saved));
			return juce::File(saved);
		}
	}
	if (const juce::File drop = legacyAppSupportDir().getChildFile("roms"); isValidRomDir(drop))
	{
		persistRomDir(drop);
		return drop;
	}
	return {};
}

inline void persistRomDir(const juce::File &dir)
{
	juce::PropertiesFile props(settingsOptions());
	props.setValue("romPath", dir.getFullPathName());
	props.saveIfNeeded();
}

// NVRAM home (sysram patch bank + globals; MAME reads AND REWRITES it on every exit).
// The plugin always runs against its own per-user copy so sessions never mutate the
// user's original dump: on first boot the sysram is seeded from an `nvram` sibling of
// the ROM dir (the usual dumped-together layout) if one exists. PROPHECY_NVRAM
// overrides everything (and IS used in place — explicit means explicit). A missing
// sysram is survivable: the firmware re-initializes an empty battery-backed RAM.
inline juce::File nvramDirFor(const juce::File &romDir)
{
	if (const char *env = std::getenv("PROPHECY_NVRAM"); env && *env)
		return juce::File{juce::String::fromUTF8(env)};
	juce::File own = appSupportDir().getChildFile("nvram");
	const juce::File ownSys = own.getChildFile("korgprop").getChildFile("sysram");
	if (!ownSys.existsAsFile())
	{
		const juce::File legacy = legacyAppSupportDir().getChildFile("nvram")
			.getChildFile("korgprop").getChildFile("sysram");
		const juce::File sibling = romDir.getParentDirectory().getChildFile("nvram")
			.getChildFile("korgprop").getChildFile("sysram");
		const juce::File seed = legacy.existsAsFile() ? legacy : sibling;
		if (seed.existsAsFile())
		{
			ownSys.getParentDirectory().createDirectory();
			seed.copyFileTo(ownSys);
		}
	}
	own.createDirectory();
	return own;
}

} // namespace romloc
