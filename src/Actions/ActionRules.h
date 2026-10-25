#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

class MultiMidi;
class MackieShowbox;
class Connections;
class Hid;
class FrontPanel;

class ActionRules {
public:
    void begin(MultiMidi* midiIn, MackieShowbox* showboxIn, Connections* connectionsIn, Hid* hidIn = nullptr);
    void setFrontPanel(FrontPanel* panelIn);
    void setVariable(const char* name, float value);
    void setDebugSerial(Print* serial);
    void tick();

    String json();
    bool snapshotJson(String& json, String& error);
    String varsJson();
    bool setVarJson(const String& body, String& error);
    String buildVarsJson() const;
    bool applySetVar(const String& body, String& error);
    bool saveJson(const String& body, String& error);
    bool removeRule(int id, String& error);
    bool runRule(int id, String& error);

    bool claimsTrs(const char* jack, const char* gesture) const;
    bool claimsMidi(const char* kind, uint8_t channel, uint8_t number) const;
    bool claimsPanel(const char* button, const char* gesture) const;
    void onMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value);
    void onPanel(const char* button, const char* gesture);
    void onShowbox(const char* entity, const char* value);
    void onPacket(const uint8_t* data, size_t length, const char* dir);
    void noteEntityWrite(const char* entityName, const char* value);
    float getVariable(const char* name) const;

private:
    struct Rule {
        uint16_t id = 0;
        bool on = true;
        String name;
        String json;
    };
    struct LocalVar {
        char name[20] = {};
        float value = 0;
    };
    struct Job {
        bool active = false;
        uint16_t id = 0;
        uint16_t step = 0;
        uint32_t waitUntil = 0;
        uint8_t moveKind = 0;
        uint16_t x0 = 0;
        uint16_t y0 = 0;
        uint16_t x1 = 0;
        uint16_t y1 = 0;
        uint32_t moveStart = 0;
        uint32_t moveDur = 0;
        uint8_t moveButtons = 0;
        int16_t lastMoveX = 0;
        int16_t lastMoveY = 0;
        uint16_t lastAbsX = 0;
        uint16_t lastAbsY = 0;
        uint8_t looperKind = 0;
        uint8_t looperStep = 0;
        char triggerValue[16] = {};
        LocalVar locals[8] = {};
        uint8_t localCount = 0;
    };
    struct StoredVar {
        char name[20] = {};
        float value = 0;
        bool persist = false;
    };
    enum class Ask : uint8_t { None, Save, Remove, Run, List, Vars, SetVar };

    void load();
    void flush();
    void seedBuiltins();
    void patchBuiltinMeta();
    bool hasBuiltin(const char* key) const;
    void loadVars();
    void saveVars();
    float getVar(Job* job, const String& name) const;
    void setVar(Job* job, const String& name, float value, bool local, bool persist);
    void applyGainBoost(Job& job, JsonObjectConst step);
    void dispatchLooper(const String& op);
    void startLooper(Job& job, const String& op);
    void runLooper(Job& job);
    void runSystem(const String& command);
    bool applySave(const String& body, String& error);
    bool applyRemove(int id, String& error);
    String buildJson() const;
    bool waitAsk(Ask kind, String& error);
    int indexOf(int id) const;
    bool enqueue(uint16_t id, const char* triggerValue = "");
    void pump();
    bool runStep(Job& job, JsonObjectConst step);
    void watchConnections();
    void watchJacks();
    void drainMidi();
    void drainEntityEvents();
    bool linkUp(const String& link) const;
    bool triggerMatches(JsonObjectConst trigger, const char* event, const char* p1, const char* p2, const char* p3, const char* p4, const char* p5) const;
    bool conditionPass(JsonVariantConst node) const;
    bool leafPass(JsonObjectConst leaf) const;
    void fire(const char* event, const char* p1, const char* p2, const char* p3, const char* p4, const char* p5 = "");
    static void jackPress(void* parameter);
    static void jackLong(void* parameter);
    static void jackShort(void* parameter);
    static void jackLongUp(void* parameter);
    static void jackDouble(void* parameter);

    void noteMidiEcho(uint8_t cc, uint8_t channel);
    bool isMidiEcho(uint8_t channel, uint8_t number) const;
    bool wasRecentMidiCc(uint8_t cc, uint8_t channel) const;

    MultiMidi* midi = nullptr;
    MackieShowbox* showbox = nullptr;
    Connections* connections = nullptr;
    Hid* hid = nullptr;
    FrontPanel* panel = nullptr;
    Print* debug = nullptr;
    StoredVar globals[24] = {};
    uint8_t globalCount = 0;
    bool varsDirty = false;
    std::vector<Rule> rules;
    std::vector<Job> jobs;
    std::vector<uint16_t> storedIds;
    bool seeded = false;
    bool hostUp = false;
    bool peripheralUp = false;
    bool usbUp = false;
    bool inAction = false;
    uint32_t inActionSinceMs = 0;
    uint32_t ignoreMidiUntil = 0;
    uint32_t ignoreMidiEchoUntil = 0;
    uint32_t gatedSinceMs = 0;
    uint8_t ignoreMidiEchoCc = 0xFF;
    uint8_t ignoreMidiEchoCh = 0xFF;
    uint32_t lastMidiInUntil = 0;
    uint8_t lastMidiInCc = 0xFF;
    uint8_t lastMidiInCh = 0xFF;
    uint32_t looperChangedAtMs = 0;
    uint32_t looperTapWatch = 0;
    uint8_t looperTapStateBefore = 0xFF;
    char pendingLooperOp[8] = {};
    uint32_t pendingLooperDueMs = 0;
    uint32_t ignoreShowboxUntil = 0;
    uint32_t readyDumpSeq = 0;
    float lastOwnGainWrite = -9999.0f;
    int lastOwnGainEntity = -1;
    uint32_t lastOwnGainWriteMs = 0;
    void noteOwnGainWrite(int id, float value);
    void* jacks[8] = {};
    bool dirty = false;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    volatile Ask ask = Ask::None;
    volatile bool askBusy = false;
    volatile bool askDone = false;
    volatile bool askOk = false;
    String askIn;
    int askId = 0;
    String askOut;
};

bool actionRulesClaimTrs(const char* jack, const char* gesture);
bool actionRulesClaimMidi(const char* kind, uint8_t channel, uint8_t number);
bool actionRulesClaimPanel(const char* button, const char* gesture);
void actionRulesOnMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value);
void actionRulesOnPanel(const char* button, const char* gesture);
float actionRulesVariable(const char* name);
void actionRulesNoteEntityWrite(const char* entityName, const char* value);
