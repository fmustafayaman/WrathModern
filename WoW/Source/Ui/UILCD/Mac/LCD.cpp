#include <cstdint>
#include "WoW/Source/Ui/UILCD/LCD.h"

// Logitech G-series keyboard displays have no macOS driver. The client behaves as the Windows client
// does without such a keyboard: the display is never initialized and every notification is dropped.

CEzLcd* CGLCD::m_lcd;
int32_t CGLCD::m_chatAlertLines;
uint8_t CGLCD::initComplete;
CGPlayer_C* CGLCD::m_player;
uint8_t CGLCD::m_inWorld;
TSHashTable<LCDHANDLE, HASHKEY_CONSTSTRI> CGLCD::m_handles;
TSList<MAPDATA, TSGetLink<MAPDATA> > CGLCD::m_mapData;
TSList<POIIcon, TSGetLink<POIIcon> > CGLCD::m_POIIcons;

void CGLCD::Initialize() {
    initComplete = 0;
}

void CGLCD::Shutdown() {
}

void CGLCD::Login() {
}

void CGLCD::Logout() {
}

void CGLCD::EnterWorld() {
    m_inWorld = 1;
}

void CGLCD::LeaveWorld() {
    m_inWorld = 0;
}

void CGLCD::ClearWaitQueueScreen() {
}

void CGLCD::IdleDisconnectWarning() {
}

void CGLCD::ShowBattlefieldScreen() {
}

void CGLCD::HideBattlefieldAlert() {
}

void CGLCD::OnBattlefieldReady(const BattlefieldStatus* status) {
}

void CGLCD::OnBattlefieldActive() {
}

void CGLCD::OnWhisper(const char* name) {
}

void CGLCD::AuctionOutbid(const char* name) {
}

void CGLCD::AuctionExpired(const char* name) {
}

void CGLCD::AuctionSold(const char* name) {
}

void CGLCD::AuctionWon(const char* name) {
}
