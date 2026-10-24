#ifndef WOW_SOURCE_AUTOLOGIN_H
#define WOW_SOURCE_AUTOLOGIN_H

// Unattended login for protocol testing, enabled with WRATHMODERN_AUTOLOGIN=1. Credentials come
// from a user-only file outside the repository; nothing is logged but the account name.
void AutologinInitialize();

// Observe real character-list/world transitions. A packet alone is not proof that the native
// world is playable; the optional world-context observer also records the active player,
// connection and actual presented-frame count in Logs/session-health.log.
void AutologinWorldInitialize();
void AutologinNotifyCharacterList();
void AutologinNotifyWorldVerified();

#endif
