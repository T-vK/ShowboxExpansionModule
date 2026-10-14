#pragma once

#include <Arduino.h>

class MackieShowbox;
class FrontPanel;
class MultiMidi;

void lanKeepMdns();
void lanQueueMdnsRefresh();
void lanSetMultiMidi(MultiMidi* mm);
void lanPreferWifiAirtime();
void lanPreferWifiWithBle();
void lanRefreshPhy();
void lanNoteBleLive(bool on);
bool lanBleLive();
void lanNoteWsClients(int n);
void lanNoteWsRx();
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

extern volatile bool gMdnsUp;
extern volatile bool gSetupDone;
