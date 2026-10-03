// wt_cfgdump.exe: weapon_tech.dll's cfg parsing, offline. Reads a cfg (flat or format v2) with the DLL's own code
// (ReadCfgText -> ParseWeaponTechText -> [features] gates) and writes the cfg_dump state file, so two cfgs can be
// compared without starting the game (tools\cfg_migrate.py --check runs it on the old and the new file).
//   wt_cfgdump <weapon_tech.cfg> <state out> [<normalised lines out>]
// The parse log (bad lines, unknown keys, the feature summary) goes to stderr.
// Build: cl /nologo /O2 /MT /EHsc /std:c++17 wt_cfgdump.cpp /Fe:wt_cfgdump.exe /link kernel32.lib
#include "weapon_tech.h"

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: wt_cfgdump <weapon_tech.cfg> <state out> [<normalised lines out>]\n");
		return 2;
	}
	g_log = stderr;
	char *text = ReadCfgText(argv[1]);
	if (!text)
	{
		fprintf(stderr, "can't read %s\n", argv[1]);
		return 1;
	}
	ParseWeaponTechText(text);
	ApplyFeatureGates();
	LogFeatureSummary();
	FILE *f = fopen(argv[2], "w");
	if (!f)
		return 1;
	uint64_t hash = 0;
	WriteCfgStateDump(f, &hash);
	fclose(f);
	if (argc > 3)
		if (FILE *n = fopen(argv[3], "w"))
		{
			fputs(text, n);
			fclose(n);
		}
	printf("%016llx lines %d bad %d unknown %d\n", static_cast<unsigned long long>(hash), g_wtLines, g_wtBadLines, g_wtUnknownLines);
	free(text);
	return g_wtBadLines ? 3 : 0;
}
