#ifndef MDFN_SETTINGS_H
#define MDFN_SETTINGS_H

extern bool setting_pce_fast_forcesgx;
extern bool setting_pce_fast_nospritelimit;
extern int setting_initial_scanline;
extern int setting_last_scanline;
extern int setting_pce_overclocked;
extern int setting_pce_hoverscan;
extern int setting_pce_fast_cddavolume;
extern int setting_pce_fast_adpcmvolume;
extern int setting_pce_fast_cdpsgvolume;
extern uint32_t setting_pce_fast_cdspeed;
extern std::string setting_pce_fast_cdbios;
extern bool OrderOfGriffonFix;

bool MDFN_LoadSettings(const char *path, const char *section = NULL, bool override = false);
bool MDFN_MergeSettings(const void*);
bool MDFN_MergeSettings(const std::vector<void> &);
bool MDFN_SaveSettings(const char *path);

void MDFN_KillSettings(void);	// Free any resources acquired.

// This should assert() or something if the setting isn't found, since it would
// be a totally tubular error!
uint64 MDFN_GetSettingUI(const char *name);
int64 MDFN_GetSettingI(const char *name);
double MDFN_GetSettingF(const char *name);
bool MDFN_GetSettingB(const char *name);
std::string MDFN_GetSettingS(const char *name);
#endif
