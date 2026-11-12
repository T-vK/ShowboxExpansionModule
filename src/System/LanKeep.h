#pragma once

#include <Arduino.h>

class MackieShowbox;
class FrontPanel;
class MultiMidi;
class CaptivePortal;

void lanKeepMdns();
void lanQueueMdnsRefresh();
void lanSetMultiMidi(MultiMidi* mm);
void lanSetCaptivePortal(CaptivePortal* portal);
void lanPreferWifiAirtime();
void lanPreferWifiWithBle();
// Pairing window: BLE adverts must land inside a 2s scan. Wi-Fi-first coex
// stretches a 400 ms advert to several seconds while SoftAP is up.
void lanSetPairingAir(bool on);
void lanRefreshPhy();
void lanNoteBleLive(bool on);
bool lanBleLive();
void lanNoteWsClients(int n);
void lanNoteWsRx();
void lanSetWsRemotes(const uint16_t* ports, int n);
int lanWsClients();
bool lanWsLive();
void lanNoteHttp();
void lanNoteHttpBody(bool on);
void lanNoteBigHttp(bool on);
void lanTouchBigHttp();
bool lanHttpBodyBusy();
bool lanHoldMidi();
uint32_t lanHttpQuietMs();
uint32_t lanFirstHttpMs();
void lanKickArp();
void lanRecoverUnicast();
void lanRejoinSta();
void lanTickRejoin();
void lanTickUnicastHealth();
void lanTickHeapGuard();
void lanRequestHttpRestart();
bool lanTakeHttpRestart();
void lanRestoreBleSleepIfIdle();
void lanInstallWifiEvents();
void lanKeepWifi(const FrontPanel& panel);
void lanTickIsoReboot(MackieShowbox& showbox);
void lanTickSoak(MackieShowbox& showbox);
uint32_t lanGwRttMs();
int lanGwMisses();
void lanTickHttpHealth();
// Drop any lwIP PCB still holding TCP port 80 so AsyncServer::begin can bind
// after a failed rebind (EADDRINUSE / -8 while the listen list looks empty).
void lanReleaseHttpPort();
void lanNoteApActive(bool on);
// SoftAP torn down outside CaptivePortal. Portal clears apUp and retries.
// kind: 1 = HTTP wedge (long backoff), 2 = yield for STA SPA TX (short).
void lanNoteApForcedDown();
void lanNoteApYieldForStaTx();
uint8_t lanTakeApForcedDown();
// Drop a live BLE host before softAP() so SoftAP+STA does not wedge HTTP.
// Returns true when a host was dropped (caller should brief-delay then continue).
bool lanYieldBleForSoftAp();
// Quiet LanKeep's WiFi.reconnect() while WiFi.begin() is associating.
void lanNoteStaJoin(uint32_t quietMs = 20000);
bool lanStaJoinQuiet();

extern volatile bool gMdnsUp;
extern volatile bool gSetupDone;
