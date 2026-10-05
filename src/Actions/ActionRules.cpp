#include "ActionRules.h"

#include "Connections/Connections.h"
#include "MackieShowbox/MackieShowbox.h"
#include "MultiMidi/MultiMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include "pin_definitions.h"
#include "string_mappings.h"
#include <ArduinoJson.h>
#include <OneButton.h>
#include <Preferences.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
ActionRules* instance = nullptr;
const char* kJacks[] = {"1t", "1r", "2t", "2r", "3t", "3r", "4t", "4r"};
const uint8_t kPins[] = {TRS1_TIP, TRS1_RING, TRS2_TIP, TRS2_RING, TRS3_TIP, TRS3_RING, TRS4_TIP, TRS4_RING};
const char* kGestures[] = {"press", "long", "shortRelease", "longRelease", "double"};
const char* kEvents[] = {"trs", "midi", "connect", "showbox"};
const char* kMidiKinds[] = {"noteOn", "noteOff", "cc"};
const char* kConnects[] = {"bleHost", "blePeripheral", "usbMidi"};
const char* kEdges[] = {"up", "down"};
const char* kStepTypes[] = {
    "midi", "hidScroll", "hidKey", "hidMouse", "hidTouch", "sleep", "showbox", "looper", "snapshot"
};
const char* kKeys[] = {"pageup", "pagedown", "up", "down", "left", "right", "space", "enter", "escape", "tab"};
const char* kTouch[] = {"tap", "swipe", "pinch-in", "pinch-out"};
const char* kLooper[] = {"down", "up", "double", "long"};
const char* kOps[] = {"eq", "ne", "gt", "lt"};
constexpr int kStepTypeCount = sizeof(kStepTypes) / sizeof(kStepTypes[0]);

int clampInt(int value, int lo, int hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

bool oneOf(const String& value, const char* const* options, int count) {
    for (int i = 0; i < count; i++) {
        if (value == options[i]) {
            return true;
        }
    }
    return false;
}

bool blank(const String& value) {
    return value.length() == 0 || value == "*";
}

bool numberOrAny(const String& field, int value) {
    if (blank(field)) {
        return true;
    }
    return field.toInt() == value;
}

String jsonText(JsonVariantConst value) {
    if (value.isNull()) {
        return "";
    }
    return value.as<String>();
}

int jsonInt(JsonObject object, const char* key, int fallback) {
    String raw = jsonText(object[key]);
    if (raw.length() == 0) {
        return fallback;
    }
    return raw.toInt();
}

int jackIndex(const String& jack) {
    for (int i = 0; i < 8; i++) {
        if (jack == kJacks[i]) {
            return i;
        }
    }
    return -1;
}

void entityHeard(entity_id id, const char* value) {
    if (instance == nullptr) {
        return;
    }
    auto name = entity_id_to_string.find(id);
    if (name == entity_id_to_string.end()) {
        return;
    }
    instance->onShowbox(name->second.c_str(), value == nullptr ? "" : value);
}

const char* take(const char* text, char separator, char* out, size_t size) {
    size_t n = 0;
    if (text == nullptr) {
        text = "";
    }
    while (*text != '\0' && *text != separator && *text != '\n' && *text != '\r') {
        if (n + 1 < size) {
            out[n++] = *text;
        }
        text++;
    }
    out[n] = '\0';
    if (*text == separator) {
        return text + 1;
    }
    return text;
}
}

bool actionRulesClaimTrs(const char* jack, const char* gesture) {
    return instance != nullptr && instance->claimsTrs(jack, gesture);
}

void actionRulesOnMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value) {
    if (instance != nullptr) {
        instance->onMidi(kind, channel, number, value);
    }
}

void ActionRules::jackPress(void* parameter) {
    int index = (int)(intptr_t)parameter;
    if (instance != nullptr && index >= 0 && index < 8) {
        instance->fire("trs", kJacks[index], "press", "", "");
    }
}
void ActionRules::jackLong(void* parameter) {
    int index = (int)(intptr_t)parameter;
    if (instance != nullptr && index >= 0 && index < 8) {
        instance->fire("trs", kJacks[index], "long", "", "");
    }
}
void ActionRules::jackShort(void* parameter) {
    int index = (int)(intptr_t)parameter;
    if (instance != nullptr && index >= 0 && index < 8) {
        instance->fire("trs", kJacks[index], "shortRelease", "", "");
    }
}
void ActionRules::jackLongUp(void* parameter) {
    int index = (int)(intptr_t)parameter;
    if (instance != nullptr && index >= 0 && index < 8) {
        instance->fire("trs", kJacks[index], "longRelease", "", "");
    }
}
void ActionRules::jackDouble(void* parameter) {
    int index = (int)(intptr_t)parameter;
    if (instance != nullptr && index >= 0 && index < 8) {
        instance->fire("trs", kJacks[index], "double", "", "");
    }
}

void ActionRules::begin(MultiMidi* midiIn, MackieShowbox* showboxIn, Connections* connectionsIn) {
    midi = midiIn;
    showbox = showboxIn;
    connections = connectionsIn;
    instance = this;
    load();
    if (showbox != nullptr) {
        showbox->entityListener = entityHeard;
    }
    for (int i = 0; i < 8; i++) {
        OneButton* button = new OneButton(kPins[i], true);
        void* token = (void*)(intptr_t)i;
        button->attachPress(jackPress, token);
        button->attachLongPressStart(jackLong, token);
        button->attachClick(jackShort, token);
        button->attachLongPressStop(jackLongUp, token);
        button->attachDoubleClick(jackDouble, token);
        jacks[i] = button;
    }
}

void ActionRules::setDebugSerial(Print* serial) {
    debug = serial;
}

void ActionRules::tick() {
    if (ask != Ask::None && !askDone) {
        askOut = "";
        if (ask == Ask::Save) {
            askOk = applySave(askIn, askOut);
        } else if (ask == Ask::Remove) {
            askOk = applyRemove(askId, askOut);
        } else if (ask == Ask::Run) {
            askOk = enqueue((uint16_t)askId);
            if (!askOk) {
                askOut = "That reaction is gone.";
            }
        } else if (ask == Ask::List) {
            askOut = buildJson();
            askOk = true;
        }
        if (dirty) {
            flush();
        }
        askDone = true;
    } else if (dirty) {
        flush();
    }
    watchJacks();
    watchConnections();
    pump();
}

bool ActionRules::waitAsk(Ask kind, String& error) {
    portENTER_CRITICAL(&lock);
    if (askBusy) {
        portEXIT_CRITICAL(&lock);
        error = "The module is busy with another reaction.";
        return false;
    }
    askBusy = true;
    askDone = false;
    askOk = false;
    portEXIT_CRITICAL(&lock);
    ask = kind;
    uint32_t start = millis();
    while (!askDone && (int32_t)(millis() - start) < 3000) {
        delay(1);
    }
    if (!askDone) {
        ask = Ask::None;
        askBusy = false;
        error = "The module did not answer.";
        return false;
    }
    error = askOut;
    bool ok = askOk;
    ask = Ask::None;
    askBusy = false;
    return ok;
}

bool ActionRules::snapshotJson(String& json, String& error) {
    if (!waitAsk(Ask::List, error)) {
        json = "";
        return false;
    }
    json = error;
    error = "";
    return true;
}

String ActionRules::json() {
    String json;
    String error;
    if (!snapshotJson(json, error)) {
        return String();
    }
    return json;
}

bool ActionRules::saveJson(const String& body, String& error) {
    askIn = body;
    askId = 0;
    return waitAsk(Ask::Save, error);
}

bool ActionRules::removeRule(int id, String& error) {
    askId = id;
    return waitAsk(Ask::Remove, error);
}

bool ActionRules::runRule(int id, String& error) {
    askId = id;
    return waitAsk(Ask::Run, error);
}

void ActionRules::watchJacks() {
    for (int i = 0; i < 8; i++) {
        if (jacks[i] != nullptr) {
            static_cast<OneButton*>(jacks[i])->tick();
        }
    }
}

bool ActionRules::linkUp(const String& link) const {
    if (link == "bleHost") {
        return midi != nullptr && midi->hostConnected();
    }
    if (link == "blePeripheral") {
        return connections != nullptr && connections->bleIncomingOn();
    }
    if (link == "usbMidi") {
        const char* usbStatus = UsbMidiHost::status();
        return usbStatus != nullptr && strstr(usbStatus, "device connected") != nullptr;
    }
    return false;
}

void ActionRules::watchConnections() {
    bool host = linkUp("bleHost");
    bool peripheral = linkUp("blePeripheral");
    bool usb = linkUp("usbMidi");
    if (!seeded) {
        hostUp = host;
        peripheralUp = peripheral;
        usbUp = usb;
        seeded = true;
        return;
    }
    if (host != hostUp) {
        hostUp = host;
        fire("connect", "bleHost", host ? "up" : "down", "", "");
    }
    if (peripheral != peripheralUp) {
        peripheralUp = peripheral;
        fire("connect", "blePeripheral", peripheral ? "up" : "down", "", "");
    }
    if (usb != usbUp) {
        usbUp = usb;
        fire("connect", "usbMidi", usb ? "up" : "down", "", "");
    }
}

void ActionRules::onMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value) {
    if (inAction || (int32_t)(millis() - ignoreMidiUntil) < 0) {
        return;
    }
    char channelText[8];
    char numberText[8];
    char valueText[8];
    snprintf(channelText, sizeof(channelText), "%u", channel + 1);
    snprintf(numberText, sizeof(numberText), "%u", number);
    snprintf(valueText, sizeof(valueText), "%u", value);
    fire("midi", kind, channelText, numberText, valueText);
}

void ActionRules::onShowbox(const char* entity, const char* value) {
    if (inAction || (int32_t)(millis() - ignoreShowboxUntil) < 0) {
        return;
    }
    fire("showbox", entity, value == nullptr ? "" : value, "", "");
}

bool ActionRules::claimsTrs(const char* jack, const char* gesture) const {
    if (jack == nullptr || gesture == nullptr) {
        return false;
    }
    for (const Rule& rule : rules) {
        if (!rule.on) {
            continue;
        }
        for (const Field& trigger : rule.triggers) {
            if (trigger.type == "trs" && trigger.a == jack && trigger.b == gesture) {
                return true;
            }
        }
    }
    return false;
}

bool ActionRules::triggerMatches(const Field& trigger, const char* event, const char* p1, const char* p2, const char* p3, const char* p4) const {
    if (trigger.type != event) {
        return false;
    }
    if (trigger.type == "trs" || trigger.type == "connect") {
        return trigger.a == p1 && trigger.b == p2;
    }
    if (trigger.type == "midi") {
        if (!blank(trigger.a) && trigger.a != p1) {
            return false;
        }
        return numberOrAny(trigger.b, atoi(p2)) && numberOrAny(trigger.c, atoi(p3)) && numberOrAny(trigger.d, atoi(p4));
    }
    if (trigger.a != p1) {
        return false;
    }
    if (blank(trigger.b)) {
        return true;
    }
    if (trigger.b == p2) {
        return true;
    }
    char* endA = nullptr;
    char* endB = nullptr;
    float left = strtof(trigger.b.c_str(), &endA);
    float right = strtof(p2, &endB);
    return endA != trigger.b.c_str() && endB != p2 && fabsf(left - right) < 0.05f;
}

bool ActionRules::conditionsPass(const Rule& rule) const {
    for (const Field& condition : rule.conditions) {
        if (condition.type == "connection") {
            bool up = linkUp(condition.a);
            if ((condition.b == "up") != up) {
                return false;
            }
            continue;
        }
        if (condition.type == "trs") {
            int index = jackIndex(condition.a);
            if (index < 0) {
                return false;
            }
            bool pressed = digitalRead(kPins[index]) == LOW;
            bool wantPressed = condition.b == "down" || condition.b == "pressed";
            if (pressed != wantPressed) {
                return false;
            }
            continue;
        }
        if (condition.type != "showbox" || showbox == nullptr) {
            return false;
        }
        auto found = string_to_entity_id.find(std::string(condition.a.c_str()));
        if (found == string_to_entity_id.end()) {
            return false;
        }
        entity_id id = found->second;
        float actual = 0;
        entity_data_type kind = entity_type_mapping[id];
        if (kind == BOOL) {
            actual = showbox->getBoolEntityValue(id) ? 1.0f : 0.0f;
        } else if (kind == UINT8) {
            actual = showbox->getUint8EntityValue(id);
        } else {
            actual = showbox->getFloatEntityValue(id);
        }
        String raw = condition.c;
        raw.toLowerCase();
        float wanted = 0;
        if (raw == "true" || raw == "on") {
            wanted = 1;
        } else if (raw == "false" || raw == "off") {
            wanted = 0;
        } else {
            wanted = raw.toFloat();
        }
        String op = condition.b.length() == 0 ? "eq" : condition.b;
        bool pass = false;
        if (op == "gt") {
            pass = actual > wanted;
        } else if (op == "lt") {
            pass = actual < wanted;
        } else if (op == "ne") {
            pass = fabsf(actual - wanted) >= 0.05f;
        } else {
            pass = fabsf(actual - wanted) < 0.05f;
        }
        if (!pass) {
            return false;
        }
    }
    return true;
}

void ActionRules::fire(const char* event, const char* p1, const char* p2, const char* p3, const char* p4) {
    for (const Rule& rule : rules) {
        if (!rule.on) {
            continue;
        }
        bool matched = false;
        for (const Field& trigger : rule.triggers) {
            if (triggerMatches(trigger, event, p1, p2, p3, p4)) {
                matched = true;
                break;
            }
        }
        if (!matched || !conditionsPass(rule)) {
            continue;
        }
        if (enqueue(rule.id) && debug != nullptr) {
            debug->printf("Reaction %u%s%s\n", rule.id, rule.name.length() ? " " : "", rule.name.c_str());
        }
    }
}

bool ActionRules::enqueue(uint16_t id) {
    if (indexOf(id) < 0) {
        return false;
    }
    Job job;
    job.active = true;
    job.id = id;
    job.step = 0;
    job.waitUntil = 0;
    jobs.push_back(job);
    return true;
}

void ActionRules::pump() {
    for (Job& job : jobs) {
        if (!job.active) {
            continue;
        }
        int slot = indexOf(job.id);
        if (slot < 0) {
            job.active = false;
            continue;
        }
        const Rule& rule = rules[slot];
        for (int burst = 0; burst < 64 && job.active; burst++) {
            if ((int32_t)(millis() - job.waitUntil) < 0) {
                break;
            }
            if (job.step >= rule.actions.size()) {
                job.active = false;
                break;
            }
            Field step = rule.actions[job.step++];
            if (step.type == "sleep") {
                long ms = step.a.toInt();
                if (ms < 0) {
                    ms = 0;
                }
                job.waitUntil = millis() + (uint32_t)ms;
                break;
            }
            runStep(step);
        }
    }
    if (jobs.size() > 32) {
        std::vector<Job> kept;
        for (const Job& job : jobs) {
            if (job.active) {
                kept.push_back(job);
            }
        }
        jobs.swap(kept);
    }
}

void ActionRules::runStep(const Field& step) {
    inAction = true;
    String error;
    if (step.type == "midi" && midi != nullptr) {
        int channel = step.b.toInt();
        if (channel < 1) {
            channel = 1;
        }
        if (channel > 16) {
            channel = 16;
        }
        uint8_t number = (uint8_t)step.c.toInt();
        uint8_t value = (uint8_t)step.d.toInt();
        int8_t wire = (int8_t)(channel - 1);
        ignoreMidiUntil = millis() + 40;
        if (step.a == "noteOn") {
            midi->noteOn(number, value, wire);
        } else if (step.a == "noteOff") {
            midi->noteOff(number, value, wire);
        } else {
            midi->controlChange(number, value, wire);
        }
    } else if (step.type == "hidScroll" && connections != nullptr) {
        int amount = step.a.toInt();
        int sent = 0;
        while (amount != 0 && sent < 32) {
            int chunk = amount > 127 ? 127 : (amount < -127 ? -127 : amount);
            connections->queueScroll(chunk, error);
            amount -= chunk;
            sent++;
        }
    } else if (step.type == "hidKey" && connections != nullptr) {
        connections->queueKey(step.a.c_str(), error);
    } else if (step.type == "hidMouse" && connections != nullptr) {
        connections->queueMouse(step.a.toInt(), step.b.toInt(), step.c.toInt(), error, step.d.toInt());
    } else if (step.type == "hidTouch" && connections != nullptr) {
        connections->queueTouch(step.a, step.b.toInt(), step.c.toInt(), step.d.toInt(), step.e.toInt(), error);
    } else if (step.type == "showbox" && showbox != nullptr) {
        auto found = string_to_entity_id.find(std::string(step.a.c_str()));
        if (found != string_to_entity_id.end()) {
            entity_id id = found->second;
            entity_data_type kind = entity_type_mapping[id];
            ignoreShowboxUntil = millis() + 80;
            if (kind == BOOL) {
                String raw = step.b;
                raw.toLowerCase();
                bool on = raw == "1" || raw == "true" || raw == "on";
                showbox->setEntityValue(id, on);
            } else if (kind == UINT8) {
                showbox->setEntityValue(id, (uint8_t)step.b.toInt());
            } else {
                showbox->setEntityValue(id, step.b.length() == 0 ? 0.0f : step.b.toFloat());
            }
        }
    } else if (step.type == "looper" && showbox != nullptr) {
        looper_button_action action = looper_button_action::DOWN;
        if (step.a == "up") {
            action = looper_button_action::UP;
        } else if (step.a == "double") {
            action = looper_button_action::DOUBLE_PRESS;
        } else if (step.a == "long") {
            action = looper_button_action::LONG_PRESS;
        }
        showbox->sendLooperButtonAction(action);
    } else if (step.type == "snapshot" && showbox != nullptr) {
        int slot = step.a.toInt();
        if (slot >= 1 && slot <= 5) {
            showbox->snapshotAction(snapshot_action::RECALL, static_cast<snapshot_slot>(slot - 1));
        }
    }
    inAction = false;
    if (debug != nullptr && error.length() > 0) {
        debug->printf("Reaction step skipped: %s\n", error.c_str());
    }
}

static void addNamed(JsonObject object, const char* key, const String& value) {
    if (value.length() > 0) {
        object[key] = value;
    }
}

String ActionRules::buildJson() const {
    JsonDocument doc;
    JsonArray list = doc["automations"].to<JsonArray>();
    for (const Rule& rule : rules) {
        JsonObject item = list.add<JsonObject>();
        item["id"] = rule.id;
        item["on"] = rule.on;
        item["name"] = rule.name;
        JsonArray triggers = item["triggers"].to<JsonArray>();
        for (const Field& field : rule.triggers) {
            JsonObject entry = triggers.add<JsonObject>();
            entry["type"] = field.type;
            if (field.type == "trs") {
                addNamed(entry, "jack", field.a);
                addNamed(entry, "gesture", field.b);
            } else if (field.type == "midi") {
                addNamed(entry, "message", field.a);
                addNamed(entry, "channel", field.b);
                addNamed(entry, "number", field.c);
                addNamed(entry, "value", field.d);
            } else if (field.type == "connect") {
                addNamed(entry, "link", field.a);
                addNamed(entry, "change", field.b);
            } else {
                addNamed(entry, "entity", field.a);
                addNamed(entry, "value", field.b);
            }
        }
        JsonArray conditions = item["conditions"].to<JsonArray>();
        for (const Field& field : rule.conditions) {
            JsonObject entry = conditions.add<JsonObject>();
            entry["type"] = field.type;
            if (field.type == "showbox") {
                addNamed(entry, "entity", field.a);
                addNamed(entry, "op", field.b);
                addNamed(entry, "value", field.c);
            } else if (field.type == "connection") {
                addNamed(entry, "link", field.a);
                addNamed(entry, "state", field.b);
            } else {
                addNamed(entry, "jack", field.a);
                addNamed(entry, "state", field.b);
            }
        }
        JsonArray actions = item["actions"].to<JsonArray>();
        for (const Field& field : rule.actions) {
            JsonObject entry = actions.add<JsonObject>();
            entry["type"] = field.type;
            if (field.type == "midi") {
                addNamed(entry, "message", field.a);
                addNamed(entry, "channel", field.b);
                addNamed(entry, "number", field.c);
                addNamed(entry, "value", field.d);
            } else if (field.type == "hidScroll") {
                addNamed(entry, "amount", field.a);
            } else if (field.type == "hidKey") {
                addNamed(entry, "key", field.a);
            } else if (field.type == "hidMouse") {
                addNamed(entry, "x", field.a);
                addNamed(entry, "y", field.b);
                addNamed(entry, "buttons", field.c);
                addNamed(entry, "wheel", field.d);
            } else if (field.type == "hidTouch") {
                addNamed(entry, "gesture", field.a);
                addNamed(entry, "x", field.b);
                addNamed(entry, "y", field.c);
                addNamed(entry, "x2", field.d);
                addNamed(entry, "y2", field.e);
            } else if (field.type == "sleep") {
                addNamed(entry, "ms", field.a);
            } else if (field.type == "showbox") {
                addNamed(entry, "entity", field.a);
                addNamed(entry, "value", field.b);
            } else if (field.type == "looper") {
                addNamed(entry, "button", field.a);
            } else {
                addNamed(entry, "slot", field.a);
            }
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

bool ActionRules::readTrigger(JsonObject object, Field& field, String& error) {
    field = {};
    field.type = jsonText(object["type"]);
    if (!oneOf(field.type, kEvents, 4)) {
        error = "Unknown trigger.";
        return false;
    }
    if (field.type == "trs") {
        field.a = jsonText(object["jack"]);
        field.b = jsonText(object["gesture"]);
        if (jackIndex(field.a) < 0 || !oneOf(field.b, kGestures, 5)) {
            error = "Choose a TRS jack and a gesture.";
            return false;
        }
    } else if (field.type == "midi") {
        field.a = jsonText(object["message"]);
        field.b = jsonText(object["channel"]);
        field.c = jsonText(object["number"]);
        field.d = jsonText(object["value"]);
        if (!oneOf(field.a, kMidiKinds, 3)) {
            error = "Choose note on, note off, or a control change.";
            return false;
        }
    } else if (field.type == "connect") {
        field.a = jsonText(object["link"]);
        field.b = jsonText(object["change"]);
        if (!oneOf(field.a, kConnects, 3) || !oneOf(field.b, kEdges, 2)) {
            error = "Choose what connected and whether it came up or went down.";
            return false;
        }
    } else {
        field.a = jsonText(object["entity"]);
        field.b = jsonText(object["value"]);
        if (string_to_entity_id.find(std::string(field.a.c_str())) == string_to_entity_id.end()) {
            error = "Unknown Showbox control.";
            return false;
        }
    }
    return true;
}

bool ActionRules::readCondition(JsonObject object, Field& field, String& error) {
    field = {};
    field.type = jsonText(object["type"]);
    if (field.type == "showbox") {
        field.a = jsonText(object["entity"]);
        field.b = jsonText(object["op"]);
        field.c = jsonText(object["value"]);
        if (field.b.length() == 0) {
            field.b = "eq";
        }
        if (string_to_entity_id.find(std::string(field.a.c_str())) == string_to_entity_id.end()) {
            error = "Unknown Showbox control.";
            return false;
        }
        if (!oneOf(field.b, kOps, 4)) {
            error = "Condition must be equal, not equal, greater, or less.";
            return false;
        }
        return true;
    }
    if (field.type == "connection") {
        field.a = jsonText(object["link"]);
        field.b = jsonText(object["state"]);
        if (!oneOf(field.a, kConnects, 3) || !oneOf(field.b, kEdges, 2)) {
            error = "Choose a connection and whether it is up or down.";
            return false;
        }
        return true;
    }
    if (field.type == "trs") {
        field.a = jsonText(object["jack"]);
        field.b = jsonText(object["state"]);
        if (jackIndex(field.a) < 0 || (field.b != "down" && field.b != "up")) {
            error = "Choose a jack and whether it is held or released.";
            return false;
        }
        return true;
    }
    error = "Unknown condition.";
    return false;
}

bool ActionRules::readAction(JsonObject object, Field& field, String& error) {
    field = {};
    field.type = jsonText(object["type"]);
    if (!oneOf(field.type, kStepTypes, kStepTypeCount)) {
        error = "Unknown action.";
        return false;
    }
    if (field.type == "midi") {
        field.a = jsonText(object["message"]);
        field.b = jsonText(object["channel"]);
        field.c = jsonText(object["number"]);
        field.d = jsonText(object["value"]);
        if (!oneOf(field.a, kMidiKinds, 3)) {
            error = "Choose the MIDI message to send.";
            return false;
        }
    } else if (field.type == "hidKey") {
        field.a = jsonText(object["key"]);
        if (!oneOf(field.a, kKeys, 10)) {
            error = "Choose a key.";
            return false;
        }
    } else if (field.type == "hidScroll") {
        field.a = jsonText(object["amount"]);
        if (field.a.toInt() == 0) {
            error = "Scroll amount cannot be zero. Negative is down.";
            return false;
        }
    } else if (field.type == "hidMouse") {
        int x = clampInt(jsonInt(object, "x", 0), -127, 127);
        int y = clampInt(jsonInt(object, "y", 0), -127, 127);
        int buttons = clampInt(jsonInt(object, "buttons", 0), 0, 7);
        int wheel = clampInt(jsonInt(object, "wheel", 0), -127, 127);
        if (x == 0 && y == 0 && buttons == 0 && wheel == 0) {
            error = "Set a mouse move, button, or wheel.";
            return false;
        }
        field.a = String(x);
        field.b = String(y);
        field.c = String(buttons);
        field.d = String(wheel);
    } else if (field.type == "hidTouch") {
        field.a = jsonText(object["gesture"]);
        if (field.a.length() == 0) {
            field.a = jsonText(object["action"]);
        }
        if (field.a == "pinch") {
            field.a = "pinch-in";
        }
        if (!oneOf(field.a, kTouch, 4)) {
            error = "Choose tap, swipe, pinch in, or pinch out.";
            return false;
        }
        int x = clampInt(jsonInt(object, "x", 5000), 0, 10000);
        int y = clampInt(jsonInt(object, "y", 5000), 0, 10000);
        int x2 = clampInt(jsonInt(object, "x2", field.a == "swipe" ? 7000 : 5000), 0, 10000);
        int y2 = clampInt(jsonInt(object, "y2", 5000), 0, 10000);
        field.b = String(x);
        field.c = String(y);
        field.d = String(x2);
        field.e = String(y2);
    } else if (field.type == "sleep") {
        field.a = jsonText(object["ms"]);
        if (field.a.toInt() < 0) {
            error = "Wait cannot be negative.";
            return false;
        }
    } else if (field.type == "showbox") {
        field.a = jsonText(object["entity"]);
        field.b = jsonText(object["value"]);
        if (string_to_entity_id.find(std::string(field.a.c_str())) == string_to_entity_id.end()) {
            error = "Unknown Showbox control.";
            return false;
        }
    } else if (field.type == "looper") {
        field.a = jsonText(object["button"]);
        if (!oneOf(field.a, kLooper, 4)) {
            error = "Choose a looper button.";
            return false;
        }
    } else {
        field.a = jsonText(object["slot"]);
        int slot = field.a.toInt();
        if (slot < 1 || slot > 5) {
            error = "Snapshot slot is 1 to 5.";
            return false;
        }
    }
    return true;
}

bool ActionRules::applySave(const String& body, String& error) {
    if (body.length() > 3900) {
        error = "That reaction is too large to store.";
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        error = "That reaction is not valid JSON.";
        return false;
    }
    JsonObject object = doc.as<JsonObject>();
    Rule draft;
    draft.id = object["id"] | 0;
    if (object["on"].is<bool>()) {
        draft.on = object["on"].as<bool>();
    } else {
        String on = jsonText(object["on"]);
        on.toLowerCase();
        draft.on = on != "0" && on != "false" && on != "off";
    }
    draft.name = jsonText(object["name"]);
    for (JsonObject entry : object["triggers"].as<JsonArray>()) {
        Field field;
        if (!readTrigger(entry, field, error)) {
            return false;
        }
        draft.triggers.push_back(field);
    }
    for (JsonObject entry : object["conditions"].as<JsonArray>()) {
        Field field;
        if (!readCondition(entry, field, error)) {
            return false;
        }
        draft.conditions.push_back(field);
    }
    for (JsonObject entry : object["actions"].as<JsonArray>()) {
        Field field;
        if (!readAction(entry, field, error)) {
            return false;
        }
        draft.actions.push_back(field);
    }
    if (draft.triggers.empty()) {
        error = "Add at least one trigger.";
        return false;
    }
    if (draft.actions.empty()) {
        error = "Add at least one action.";
        return false;
    }
    int slot = draft.id == 0 ? -1 : indexOf(draft.id);
    if (draft.id != 0 && slot < 0) {
        error = "That reaction is gone.";
        return false;
    }
    if (slot < 0) {
        uint16_t next = 1;
        for (const Rule& rule : rules) {
            if (rule.id >= next) {
                next = rule.id + 1;
            }
        }
        draft.id = next;
        rules.push_back(draft);
    } else {
        draft.id = rules[slot].id;
        rules[slot] = draft;
    }
    dirty = true;
    error = buildJson();
    return true;
}

bool ActionRules::applyRemove(int id, String& error) {
    int slot = indexOf(id);
    if (slot < 0) {
        error = "That reaction is gone.";
        return false;
    }
    rules.erase(rules.begin() + slot);
    for (Job& job : jobs) {
        if (job.id == id) {
            job.active = false;
        }
    }
    dirty = true;
    error = buildJson();
    return true;
}

int ActionRules::indexOf(int id) const {
    for (size_t i = 0; i < rules.size(); i++) {
        if (rules[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

void ActionRules::load() {
    rules.clear();
    storedIds.clear();
    Preferences prefs;
    if (!prefs.begin("actions", true)) {
        return;
    }
    String ids = prefs.getString("ids", "");
    if (ids.length() > 0) {
        const char* cursor = ids.c_str();
        while (*cursor != '\0') {
            char token[8];
            cursor = take(cursor, ',', token, sizeof(token));
            int id = atoi(token);
            if (id <= 0) {
                continue;
            }
            String raw = prefs.getString(("a" + String(id)).c_str(), "");
            String error;
            if (raw.length() == 0 || !applySave(raw, error)) {
                continue;
            }
            storedIds.push_back((uint16_t)id);
        }
        dirty = false;
        prefs.end();
        return;
    }
    String legacy = prefs.getString("rules", "");
    prefs.end();
    const char* cursor = legacy.c_str();
    while (*cursor != '\0') {
        if (*cursor == '\n') {
            cursor++;
            continue;
        }
        char idText[8];
        char onText[4];
        char event[12];
        char p1[24];
        char p2[24];
        char p3[8];
        char p4[8];
        char steps[480];
        cursor = take(cursor, '|', idText, sizeof(idText));
        cursor = take(cursor, '|', onText, sizeof(onText));
        cursor = take(cursor, '|', event, sizeof(event));
        cursor = take(cursor, '|', p1, sizeof(p1));
        cursor = take(cursor, '|', p2, sizeof(p2));
        cursor = take(cursor, '|', p3, sizeof(p3));
        cursor = take(cursor, '|', p4, sizeof(p4));
        cursor = take(cursor, '\n', steps, sizeof(steps));
        JsonDocument doc;
        JsonObject rule = doc.to<JsonObject>();
        rule["id"] = atoi(idText);
        rule["on"] = onText[0] != '0';
        JsonObject trigger = rule["triggers"].to<JsonArray>().add<JsonObject>();
        trigger["type"] = event;
        if (strcmp(event, "trs") == 0) {
            trigger["jack"] = p1;
            trigger["gesture"] = p2;
        } else if (strcmp(event, "midi") == 0) {
            trigger["message"] = p1;
            trigger["channel"] = p2;
            trigger["number"] = p3;
            trigger["value"] = p4;
        } else if (strcmp(event, "connect") == 0) {
            trigger["link"] = p1;
            trigger["change"] = p2;
        } else {
            trigger["entity"] = p1;
            trigger["value"] = p2;
        }
        JsonArray actions = rule["actions"].to<JsonArray>();
        const char* stepCursor = steps;
        while (*stepCursor != '\0') {
            char chunk[80];
            stepCursor = take(stepCursor, ';', chunk, sizeof(chunk));
            if (chunk[0] == '\0') {
                continue;
            }
            char type[16];
            char a[24];
            char b[16];
            char c[8];
            char d[8];
            const char* field = chunk;
            field = take(field, ',', type, sizeof(type));
            field = take(field, ',', a, sizeof(a));
            field = take(field, ',', b, sizeof(b));
            field = take(field, ',', c, sizeof(c));
            take(field, ',', d, sizeof(d));
            JsonObject action = actions.add<JsonObject>();
            action["type"] = type;
            if (strcmp(type, "midi") == 0) {
                action["message"] = a;
                action["channel"] = b;
                action["number"] = c;
                action["value"] = d;
            } else if (strcmp(type, "hidScroll") == 0) {
                action["amount"] = a;
            } else if (strcmp(type, "hidKey") == 0) {
                action["key"] = a;
            } else if (strcmp(type, "hidMouse") == 0) {
                action["x"] = a;
                action["y"] = b;
                action["buttons"] = c;
                action["wheel"] = d;
            } else if (strcmp(type, "hidTouch") == 0) {
                action["gesture"] = a;
                action["x"] = b;
                action["y"] = c;
                action["x2"] = d;
            } else if (strcmp(type, "sleep") == 0) {
                action["ms"] = a;
            } else if (strcmp(type, "showbox") == 0) {
                action["entity"] = a;
                action["value"] = b;
            } else if (strcmp(type, "looper") == 0) {
                action["button"] = a;
            } else if (strcmp(type, "snapshot") == 0) {
                action["slot"] = a;
            }
        }
        String body;
        serializeJson(doc, body);
        String error;
        if (applySave(body, error)) {
            dirty = true;
        }
    }
}

void ActionRules::flush() {
    if (!dirty) {
        return;
    }
    Preferences prefs;
    if (!prefs.begin("actions", false)) {
        return;
    }
    String ids;
    std::vector<uint16_t> now;
    for (size_t i = 0; i < rules.size(); i++) {
        if (i > 0) {
            ids += ',';
        }
        ids += String(rules[i].id);
        now.push_back(rules[i].id);
        JsonDocument doc;
        deserializeJson(doc, buildJson());
        JsonArray list = doc["automations"].as<JsonArray>();
        String one;
        for (JsonObject item : list) {
            if ((int)item["id"] == rules[i].id) {
                serializeJson(item, one);
                break;
            }
        }
        String key = "a" + String(rules[i].id);
        if (one.length() > 0 && prefs.putString(key.c_str(), one) == 0) {
            if (debug != nullptr) {
                debug->println("Could not store a reaction.");
            }
        }
    }
    prefs.putString("ids", ids);
    for (uint16_t old : storedIds) {
        bool still = false;
        for (uint16_t id : now) {
            if (id == old) {
                still = true;
                break;
            }
        }
        if (!still) {
            prefs.remove(("a" + String(old)).c_str());
        }
    }
    prefs.remove("rules");
    storedIds = now;
    dirty = false;
    prefs.end();
}
