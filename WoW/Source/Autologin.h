#ifndef WOW_SOURCE_AUTOLOGIN_H
#define WOW_SOURCE_AUTOLOGIN_H

// Unattended login for protocol testing, enabled with WRATHMODERN_AUTOLOGIN=1. Credentials come
// from a user-only file outside the repository; nothing is logged but the account name.
void AutologinInitialize();

#endif
