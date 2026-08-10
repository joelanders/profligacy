// Plugin-host copy of MAME's Windows OSD implementation. Rename MAME's CLI
// entry point so the plugin host gets the OSD implementation without
// importing a second process entry point. This is less fragile than asking
// the vendored source to conditionally omit the function while included.
#define main profligacy_mame_unused_main
#include "winmain.cpp"
#undef main
