#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

class MultiMidi;
class MackieShowbox;
class Connections;

// A reaction is a Home Assistant style automation: any trigger, then every
// condition, then the actions in order. Stored as JSON.
class ActionRules {
public:
    void begin(MultiMidi* midiIn, MackieShowbox* showboxIn, Connections* connectionsIn);
    void setDebugSerial(Print* serial);
    void tick();

    String json();
    bool snapshotJson(String& json, String& error);
    bool saveJson(const String& body, String& error);
    bool removeRule(int id, String& error);
    bool runRule(int id, String& error);

    bool claimsTrs(const char* jack, const char* gesture) const;
    void onMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value);
    void onShowbox(const char* entity, const char* value);

private:
    struct Field {
        String type;
        String a;
        String b;
        String c;
        String d;
        String e;
    };
    struct Rule {
        uint16_t id = 0;
        bool on = true;
        String name;
        std::vector<Field> triggers;
        std::vector<Field> conditions;
        std::vector<Field> actions;
    };
    struct Job {
        bool active = false;
        uint16_t id = 0;
        uint16_t step = 0;
        uint32_t waitUntil = 0;
    };
    enum class Ask : uint8_t { None, Save, Remove, Run, List };

    void load();
    void flush();
    bool applySave(const String& body, String& error);
    bool applyRemove(int id, String& error);
    String buildJson() const;
    bool waitAsk(Ask kind, String& error);
    int indexOf(int id) const;
    bool enqueue(uint16_t id);
    void pump();
    void runStep(const Field& step);
    void watchConnections();
    void watchJacks();
    bool linkUp(const String& link) const;
    bool readTrigger(JsonObject object, Field& field, String& error);
    bool readCondition(JsonObject object, Field& field, String& error);
    bool readAction(JsonObject object, Field& field, String& error);
    bool triggerMatches(const Field& trigger, const char* event, const char* p1, const char* p2, const char* p3, const char* p4) const;
    bool conditionsPass(const Rule& rule) const;
    void fire(const char* event, const char* p1, const char* p2, const char* p3, const char* p4);
    static void jackPress(void* parameter);
    static void jackLong(void* parameter);
    static void jackShort(void* parameter);
    static void jackLongUp(void* parameter);
    static void jackDouble(void* parameter);

    MultiMidi* midi = nullptr;
    MackieShowbox* showbox = nullptr;
    Connections* connections = nullptr;
    Print* debug = nullptr;
    std::vector<Rule> rules;
    std::vector<Job> jobs;
    std::vector<uint16_t> storedIds;
    bool seeded = false;
    bool hostUp = false;
    bool peripheralUp = false;
    bool usbUp = false;
    bool inAction = false;
    uint32_t ignoreMidiUntil = 0;
    uint32_t ignoreShowboxUntil = 0;
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
void actionRulesOnMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value);
