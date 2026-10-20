#ifndef WOW_SOURCE_AUTOTEST_H
#define WOW_SOURCE_AUTOTEST_H

// Unattended test runs, driven by environment variables; nothing changes when none is set.
//
//   WRATHMODERN_REALMLIST=host     realm list server for this run (not saved to Config.wtf)
//   WRATHMODERN_REALM=name         realm to connect to (not saved)
//   WRATHMODERN_AUTOLOGIN=1        log in with the credentials file and enter the world
//   WRATHMODERN_AUTOLOGIN_FILE=f   user-only file: account line, password line
//   WRATHMODERN_CHARACTER=name     character to enter the world with (default: the first)
//   WRATHMODERN_SHOTS=a,b,...      screenshots: "glue@S" S seconds after the glue starts,
//                                  "world@S" S seconds after the world finished loading
//   WRATHMODERN_EXIT_AFTER_SHOTS=1 exit once the last screenshot is written
//
// Screenshots are written as Screenshots/autotest-<label>.tga in the game directory, from the
// client's own screenshot path (CGxDevice::DeviceReadScreenShot), so no desktop capture is needed.
// Progress goes to Logs/autotest.log; the password is never logged.
void AutotestInitialize();

#endif
