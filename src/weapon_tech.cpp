// weapon_tech.dll: the weapon tech (weapon_tech.h lists what is in it), for any map, stubbed or not.
//
// From the map's UI Lua (main/LUI thread), before the first weapon raise, e.g. from a file the level's CSC main()
// LuiLoad()s:
//   local init = require("package").loadlib([[.\usermaps\<map>\zone\weapon_tech.dll]], "init")
//   if init and init(true) == true then Engine.SetDvar("weapontech_active", 1) end
// init(true) hands back true when the weapon tech is active (the viewmodel hook is in), nil when it isn't (no cfg,
// another exe build, ...). Safe to call on every level load: the work is done once per process. Config and log:
// weapon_tech.h.
#include "weapon_tech.h"

// C export for tools (GetProcAddress): the version string, e.g. "0.1.0-alpha". Not a Lua function.
extern "C" __declspec(dllexport) const char *weapon_tech_version()
{
	return WT_VERSION_STRING;
}

// lua_CFunction: returning 1 hands back the top stack value (the argument), so no Lua API is needed.
extern "C" __declspec(dllexport) int init(void *)
{
	if (!g_wtInitialised)
	{
		HMODULE self;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		                   reinterpret_cast<LPCSTR>(&init), &self);
		char path[MAX_PATH];
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		if (char *slash = strrchr(path, '\\'))
			strcpy_s(slash + 1, path + MAX_PATH - (slash + 1), "weapon_tech.log");
		g_log = _fsopen(path, "w", _SH_DENYWR);
		// Debug aid: WEAPONTECH_CRASHLOG=1 in the game's environment logs access violations as they happen (bo3_crashlog.h).
		char crashEnv[8] = {};
		if (GetEnvironmentVariableA("WEAPONTECH_CRASHLOG", crashEnv, sizeof(crashEnv)) && crashEnv[0] == '1')
			InstallCrashLog(self);

		char dir[MAX_PATH];
		GetModuleFileNameA(self, dir, MAX_PATH);
		if (char *slash = strrchr(dir, '\\'))
			*slash = 0;
		WeaponTechInit(dir);
		// From here on the game thread only queues log lines; a writer thread writes them every 100 ms.
		if (StartLogWriter())
			Log("weapon_tech: log lines are written by a background thread from here on");
	}
	else
		Log("weapon_tech: init called again (a level load): %s", g_wtActive ? "active" : "not active");
	return g_wtActive ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(module);
	else if (reason == DLL_PROCESS_DETACH)
		LogDrain(true);  // what the writer thread hadn't written yet
	return TRUE;
}
