#ifndef WOW_SOURCE_AUTOLOGIN_H
#define WOW_SOURCE_AUTOLOGIN_H

// Unattended login for protocol testing, enabled with WRATHMODERN_AUTOLOGIN=1. Credentials come
// from a user-only file outside the repository; nothing is logged but the account name.
void AutologinInitialize();

// The server answers the first CMSG_PLAYER_LOGIN with the 192-byte module-scan challenge. Our
// emulated module list is a single entry, so Warmane sends us back to character selection; a
// second login is accepted (the official client's 1996-byte answer passes on the first). The
// unattended login notices the repeated SMSG_CHAR_ENUM and logs in again, exactly as a player
// clicking Enter World again would. Neither hook changes anything when autologin is off.
void AutologinNotifyCharacterList();
void AutologinNotifyWorldVerified();

#endif
