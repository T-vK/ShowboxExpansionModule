#include "ActionRules.h"

#include "Connections/Connections.h"
#include "FrontPanel/FrontPanel.h"
#include "LiveSocket/LiveSocket.h"
#include "MackieShowbox/MackieShowbox.h"
#include "constants.h"
#include "MultiMidi/MidiInbound.h"
#include "MultiMidi/MidiPorts.h"
#include "MultiMidi/MultiMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include "MidiCommon.h"
#include "pin_definitions.h"
#include "string_mappings.h"
#include <Hid.h>
#include <OneButton.h>
#include <Preferences.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern LiveSocket liveSocket;

namespace {
ActionRules* instance = nullptr;
const char* kJacks[] = {"1t", "1r", "2t", "2r", "3t", "3r", "4t", "4r"};
const uint8_t kPins[] = {TRS1_TIP, TRS1_RING, TRS2_TIP, TRS2_RING, TRS3_TIP, TRS3_RING, TRS4_TIP, TRS4_RING};
const char* kGestures[] = {"press", "long", "shortRelease", "longRelease", "double"};
const float kGainSteps[] = {
    Showbox::InputGain::POSITION_1, Showbox::InputGain::POSITION_2, Showbox::InputGain::POSITION_3,
    Showbox::InputGain::POSITION_4, Showbox::InputGain::POSITION_5, Showbox::InputGain::POSITION_6,
    Showbox::InputGain::POSITION_7, Showbox::InputGain::POSITION_8, Showbox::InputGain::POSITION_9,
    Showbox::InputGain::POSITION_10, Showbox::InputGain::POSITION_11, Showbox::InputGain::POSITION_12,
    Showbox::InputGain::POSITION_13, Showbox::InputGain::POSITION_14, Showbox::InputGain::POSITION_15,
    Showbox::InputGain::POSITION_16
};
constexpr int kGainStepCount = 16;
constexpr uint32_t kMidiEchoIgnoreMs = 1000;

struct KeyMap {
    const char* name;
    uint8_t usage;
    uint8_t modifier;
};
const KeyMap kKeys[] = {
    {"a", 0x04, 0}, {"b", 0x05, 0}, {"c", 0x06, 0}, {"d", 0x07, 0}, {"e", 0x08, 0},
    {"f", 0x09, 0}, {"g", 0x0A, 0}, {"h", 0x0B, 0}, {"i", 0x0C, 0}, {"j", 0x0D, 0},
    {"k", 0x0E, 0}, {"l", 0x0F, 0}, {"m", 0x10, 0}, {"n", 0x11, 0}, {"o", 0x12, 0},
    {"p", 0x13, 0}, {"q", 0x14, 0}, {"r", 0x15, 0}, {"s", 0x16, 0}, {"t", 0x17, 0},
    {"u", 0x18, 0}, {"v", 0x19, 0}, {"w", 0x1A, 0}, {"x", 0x1B, 0}, {"y", 0x1C, 0},
    {"z", 0x1D, 0},
    {"1", 0x1E, 0}, {"2", 0x1F, 0}, {"3", 0x20, 0}, {"4", 0x21, 0}, {"5", 0x22, 0},
    {"6", 0x23, 0}, {"7", 0x24, 0}, {"8", 0x25, 0}, {"9", 0x26, 0}, {"0", 0x27, 0},
    {"enter", 0x28, 0}, {"escape", 0x29, 0}, {"backspace", 0x2A, 0}, {"tab", 0x2B, 0},
    {"space", 0x2C, 0}, {"minus", 0x2D, 0}, {"equal", 0x2E, 0},
    {"f1", 0x3A, 0}, {"f2", 0x3B, 0}, {"f3", 0x3C, 0}, {"f4", 0x3D, 0},
    {"f5", 0x3E, 0}, {"f6", 0x3F, 0}, {"f7", 0x40, 0}, {"f8", 0x41, 0},
    {"f9", 0x42, 0}, {"f10", 0x43, 0}, {"f11", 0x44, 0}, {"f12", 0x45, 0},
    {"printscreen", 0x46, 0}, {"scrolllock", 0x47, 0}, {"pause", 0x48, 0},
    {"insert", 0x49, 0}, {"home", 0x4A, 0}, {"pageup", 0x4B, 0}, {"delete", 0x4C, 0},
    {"end", 0x4D, 0}, {"pagedown", 0x4E, 0}, {"right", 0x4F, 0}, {"left", 0x50, 0},
    {"down", 0x51, 0}, {"up", 0x52, 0},
    {"ctrl", 0xE0, 0}, {"shift", 0xE1, 0}, {"alt", 0xE2, 0}, {"gui", 0xE3, 0},
    {"leftctrl", 0xE0, 0}, {"leftshift", 0xE1, 0}, {"leftalt", 0xE2, 0}, {"leftgui", 0xE3, 0},
    {"rightctrl", 0xE4, 0}, {"rightshift", 0xE5, 0}, {"rightalt", 0xE6, 0}, {"rightgui", 0xE7, 0},
};

struct MediaMap {
    const char* name;
    uint16_t usage;
};
const MediaMap kMedia[] = {
    {"play", 0xB0}, {"pause", 0xB1}, {"record", 0xB2}, {"fastforward", 0xB3},
    {"rewind", 0xB4}, {"next", 0xB5}, {"prev", 0xB6}, {"stop", 0xB7},
    {"eject", 0xB8}, {"playpause", 0xCD}, {"mute", 0xE2}, {"volup", 0xE9},
    {"voldown", 0xEA}, {"brightnessup", 0x6F}, {"brightnessdown", 0x70},
    {"www", 0x221}, {"email", 0x18A}, {"calculator", 0x192}, {"explorer", 0x194},
    {"search", 0x221}, {"home", 0x223}, {"back", 0x224}, {"forward", 0x225},
    {"stopwww", 0x226}, {"refresh", 0x227}, {"bookmarks", 0x22A},
};

String jsonText(JsonVariantConst value) {
    if (value.isNull()) {
        return "";
    }
    return value.as<String>();
}

int jsonInt(JsonVariantConst value, int fallback) {
    if (value.isNull()) {
        return fallback;
    }
    if (value.is<int>() || value.is<long>() || value.is<float>()) {
        return value.as<int>();
    }
    String raw = value.as<String>();
    if (raw.length() == 0 || raw == "*") {
        return fallback;
    }
    return raw.toInt();
}

bool blank(const String& value) {
    return value.length() == 0 || value == "*";
}

bool numberOrAny(JsonVariantConst field, int value) {
    if (field.isNull()) {
        return true;
    }
    String raw = jsonText(field);
    if (blank(raw)) {
        return true;
    }
    return raw.toInt() == value;
}

int jackIndex(const String& jack) {
    for (int i = 0; i < 8; i++) {
        if (jack == kJacks[i]) {
            return i;
        }
    }
    return -1;
}

bool parseHex(const String& hex, uint8_t* out, size_t cap, size_t& n) {
    n = 0;
    int acc = -1;
    for (size_t i = 0; i < hex.length(); i++) {
        char c = hex[i];
        if (c == ' ' || c == ':' || c == '-' || c == ',') {
            continue;
        }
        int v = -1;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            v = 10 + (c - 'A');
        } else {
            return false;
        }
        if (acc < 0) {
            acc = v;
        } else {
            if (n >= cap) {
                return false;
            }
            out[n++] = static_cast<uint8_t>((acc << 4) | v);
            acc = -1;
        }
    }
    return acc < 0 && n > 0;
}

String toHex(const uint8_t* data, size_t length) {
    String out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        out += buf;
    }
    return out;
}

bool hexPrefix(const String& wanted, const String& actual) {
    if (wanted.length() == 0) {
        return true;
    }
    String a = wanted;
    String b = actual;
    a.replace(" ", "");
    a.replace(":", "");
    a.toUpperCase();
    b.toUpperCase();
    return b.startsWith(a);
}

const KeyMap* findKey(const String& name) {
    String lower = name;
    lower.toLowerCase();
    for (const KeyMap& key : kKeys) {
        if (lower == key.name) {
            return &key;
        }
    }
    return nullptr;
}

const MediaMap* findMedia(const String& name) {
    String lower = name;
    lower.toLowerCase();
    lower.replace(" ", "");
    lower.replace("-", "");
    for (const MediaMap& key : kMedia) {
        if (lower == key.name) {
            return &key;
        }
    }
    return nullptr;
}

uint8_t mouseButtons(JsonObjectConst object) {
    String raw = jsonText(object["button"]);
    raw.toLowerCase();
    if (raw == "left" || raw == "1") {
        return 1;
    }
    if (raw == "right" || raw == "2") {
        return 2;
    }
    if (raw == "middle" || raw == "4") {
        return 4;
    }
    if (raw == "none" || raw == "0" || raw.length() == 0) {
        return static_cast<uint8_t>(jsonInt(object["buttons"], 0));
    }
    return static_cast<uint8_t>(jsonInt(object["buttons"], 0));
}

uint8_t modifiersOf(JsonVariantConst value) {
    uint8_t bits = 0;
    auto add = [&](const String& name) {
        String lower = name;
        lower.toLowerCase();
        if (lower == "ctrl" || lower == "control" || lower == "leftctrl") bits |= 0x01;
        if (lower == "shift" || lower == "leftshift") bits |= 0x02;
        if (lower == "alt" || lower == "leftalt" || lower == "opt") bits |= 0x04;
        if (lower == "gui" || lower == "win" || lower == "cmd" || lower == "meta") bits |= 0x08;
        if (lower == "rightctrl") bits |= 0x10;
        if (lower == "rightshift") bits |= 0x20;
        if (lower == "rightalt") bits |= 0x40;
        if (lower == "rightgui") bits |= 0x80;
    };
    if (value.is<JsonArrayConst>()) {
        for (JsonVariantConst item : value.as<JsonArrayConst>()) {
            add(jsonText(item));
        }
    } else {
        add(jsonText(value));
    }
    return bits;
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
    if (value != nullptr) {
        liveSocket.noteEntity(name->second.c_str(), value);
    }
}

void packetHeard(const uint8_t* data, size_t length, const char* dir) {
    if (data != nullptr && length >= 4) {
        uint8_t type = data[3];
        if (type == TUNER_TOGGLE || type == TUNER_FEEDBACK || type == SD_CARD_EVENT
            || type == SNAPSHOT_MENU || type == SNAPSHOT || type == ALL_ENTITIES) {
            liveSocket.noteLink();
            if (type == ALL_ENTITIES) {
                liveSocket.flushMixer();
            }
        }
        if (type == ENTITY && length >= 6) {
            entity_id id = static_cast<entity_id>(data[5]);
            if (id == MAIN_HEADPHONE_GAIN || id == LOOPER_LEVEL) {
                return;
            }
        }
    }
    if (instance != nullptr) {
        instance->onPacket(data, length, dir);
    }
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

bool showboxValue(MackieShowbox* showbox, const String& entity, float& actual) {
    auto found = string_to_entity_id.find(std::string(entity.c_str()));
    if (found == string_to_entity_id.end() || showbox == nullptr) {
        return false;
    }
    entity_id id = found->second;
    entity_data_type kind = entity_type_mapping[id];
    if (kind == BOOL) {
        actual = showbox->getBoolEntityValue(id) ? 1.0f : 0.0f;
    } else if (kind == UINT8) {
        actual = showbox->getUint8EntityValue(id);
    } else {
        actual = showbox->getFloatEntityValue(id);
    }
    return true;
}

float wantedNumber(const String& rawIn) {
    String raw = rawIn;
    raw.toLowerCase();
    if (raw == "true" || raw == "on") {
        return 1;
    }
    if (raw == "false" || raw == "off") {
        return 0;
    }
    return raw.toFloat();
}

bool compareNumber(float actual, const String& op, float wanted) {
    if (op == "gt") return actual > wanted;
    if (op == "lt") return actual < wanted;
    if (op == "ge") return actual >= wanted;
    if (op == "le") return actual <= wanted;
    if (op == "ne") return fabsf(actual - wanted) >= 0.05f;
    return fabsf(actual - wanted) < 0.05f;
}

int nearestGainStep(float gain) {
    int best = 0;
    float bestDistance = fabsf(gain - kGainSteps[0]);
    for (int i = 1; i < kGainStepCount; i++) {
        float distance = fabsf(gain - kGainSteps[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

void copyVarName(char* dest, size_t cap, const String& name) {
    size_t n = 0;
    for (size_t i = 0; i < name.length() && n + 1 < cap; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            dest[n++] = c;
        }
    }
    dest[n] = '\0';
}

const char* kBuiltinRules[] = {
    "{\"on\":true,\"name\":\"Looper Rec / Play / Overdub\",\"group\":\"TRS Looper\",\"builtin\":\"looperTrsCycle\",\"triggers\":[{\"type\":\"trs\",\"jack\":\"1t\",\"gesture\":\"shortRelease\"}],\"actions\":[{\"type\":\"looper\",\"button\":\"cycle\"}]}",
    "{\"on\":true,\"name\":\"Looper Stop\",\"group\":\"TRS Looper\",\"builtin\":\"looperTrsStop\",\"triggers\":[{\"type\":\"trs\",\"jack\":\"1r\",\"gesture\":\"shortRelease\"}],\"actions\":[{\"type\":\"looper\",\"button\":\"stop\"}]}",
    "{\"on\":true,\"name\":\"Looper Delete\",\"group\":\"TRS Looper\",\"builtin\":\"looperTrsDelete\",\"triggers\":[{\"type\":\"trs\",\"jack\":\"1r\",\"gesture\":\"long\"}],\"actions\":[{\"type\":\"looper\",\"button\":\"delete\"}]}",
    "{\"on\":true,\"name\":\"MIDI Looper Rec / Play / Overdub\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"looperMidiCycle\",\"triggers\":[{\"type\":\"midi\",\"message\":\"cc\",\"number\":\"102\"}],\"actions\":[{\"type\":\"looper\",\"button\":\"cycle\"}]}",
    "{\"on\":true,\"name\":\"MIDI Looper Stop\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"looperMidiStop\",\"triggers\":[{\"type\":\"midi\",\"message\":\"cc\",\"number\":\"103\"}],\"actions\":[{\"type\":\"looper\",\"button\":\"stop\"}]}",
    "{\"on\":true,\"name\":\"MIDI Looper Delete\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"looperMidiDelete\",\"triggers\":[{\"type\":\"midi\",\"message\":\"cc\",\"number\":\"104\"}],\"actions\":[{\"type\":\"looper\",\"button\":\"delete\"}]}",
    "{\"on\":true,\"name\":\"MIDI Boost On\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"midiBoostOn\",\"triggers\":[{\"type\":\"midi\",\"message\":\"cc\",\"number\":\"105\"}],\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT2_GAIN\",\"on\":\"on\",\"steps\":2,\"var\":\"midiBoost\",\"persist\":true}]}",
    "{\"on\":true,\"name\":\"MIDI Boost Off\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"midiBoostOff\",\"triggers\":[{\"type\":\"midi\",\"message\":\"cc\",\"number\":\"106\"}],\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT2_GAIN\",\"on\":\"off\",\"steps\":2,\"var\":\"midiBoost\",\"persist\":true}]}",
    "{\"on\":true,\"name\":\"MIDI Boost Toggle\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"midiBoostToggle\",\"triggers\":[{\"type\":\"midi\",\"message\":\"cc\",\"number\":\"108\"}],\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT2_GAIN\",\"on\":\"toggle\",\"steps\":2,\"var\":\"midiBoost\",\"persist\":true}]}",
    "{\"on\":true,\"name\":\"Auto Boost On Play\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"autoBoostOn\",\"triggers\":[{\"type\":\"showbox\",\"entity\":\"LOOPER_STATE\",\"value\":\"3\"}],\"condition\":{\"type\":\"variable\",\"name\":\"autoBoostOff\",\"op\":\"lt\",\"value\":\"0.5\"},\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT2_GAIN\",\"on\":\"on\",\"steps\":2,\"var\":\"midiBoost\",\"persist\":true,\"echoCc\":105,\"echoChannel\":16}]}",
    "{\"on\":true,\"name\":\"Auto Boost Off\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"autoBoostOff\",\"triggers\":[{\"type\":\"showbox\",\"entity\":\"LOOPER_STATE\"}],\"condition\":{\"op\":\"and\",\"items\":[{\"type\":\"showbox\",\"entity\":\"LOOPER_STATE\",\"op\":\"ne\",\"value\":\"3\"},{\"type\":\"variable\",\"name\":\"autoBoostOff\",\"op\":\"lt\",\"value\":\"0.5\"}]},\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT2_GAIN\",\"on\":\"off\",\"steps\":2,\"var\":\"midiBoost\",\"persist\":true,\"echoCc\":106,\"echoChannel\":16}]}",
    "{\"on\":true,\"name\":\"Auto Boost Toggle\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"autoBoostToggle\",\"triggers\":[{\"type\":\"midi\",\"message\":\"cc\",\"number\":\"107\"}],\"actions\":[{\"type\":\"variable\",\"name\":\"autoBoostOff\",\"op\":\"toggle\",\"persist\":true}]}",
    "{\"on\":false,\"name\":\"Auto Boost Armed\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"autoBoostArmed\",\"triggers\":[{\"type\":\"variable\",\"name\":\"autoBoostOff\",\"value\":\"0\"}],\"condition\":{\"type\":\"showbox\",\"entity\":\"LOOPER_STATE\",\"op\":\"eq\",\"value\":\"3\"},\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT2_GAIN\",\"on\":\"on\",\"steps\":2,\"var\":\"midiBoost\",\"persist\":true,\"echoCc\":105,\"echoChannel\":16}]}",
    "{\"on\":false,\"name\":\"Auto Boost Disarmed\",\"group\":\"MIDI Looper & Boost\",\"builtin\":\"autoBoostDisarmed\",\"triggers\":[{\"type\":\"variable\",\"name\":\"autoBoostOff\",\"value\":\"1\"}],\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT2_GAIN\",\"on\":\"off\",\"steps\":2,\"var\":\"midiBoost\",\"persist\":true,\"echoCc\":106,\"echoChannel\":16}]}",
    "{\"on\":true,\"name\":\"Booster Pedal\",\"group\":\"Booster Pedal\",\"builtin\":\"pedalBoost\",\"triggers\":[{\"type\":\"trs\",\"jack\":\"2t\",\"gesture\":\"shortRelease\"}],\"actions\":[{\"type\":\"gainBoost\",\"entity\":\"INPUT4_GAIN\",\"on\":\"toggle\",\"low\":32,\"high\":52,\"var\":\"pedalBoost\",\"persist\":true}]}",
    "{\"on\":true,\"name\":\"Bluetooth Pairing (blue press)\",\"group\":\"Front Panel\",\"builtin\":\"panelBluePress\",\"triggers\":[{\"type\":\"panel\",\"button\":\"blue\",\"gesture\":\"press\"}],\"actions\":[{\"type\":\"system\",\"command\":\"pairing\"}]}",
    "{\"on\":true,\"name\":\"Bluetooth Mode Button (blue)\",\"group\":\"Front Panel\",\"builtin\":\"panelBlueLong\",\"triggers\":[{\"type\":\"panel\",\"button\":\"blue\",\"gesture\":\"long\"}],\"actions\":[{\"type\":\"system\",\"command\":\"bluetoothToggle\"}]}",
    "{\"on\":true,\"name\":\"Wifi Toggle Button (green)\",\"group\":\"Front Panel\",\"builtin\":\"panelGreenPress\",\"triggers\":[{\"type\":\"panel\",\"button\":\"green\",\"gesture\":\"press\"}],\"actions\":[{\"type\":\"system\",\"command\":\"wifiToggle\"}]}",
    "{\"on\":true,\"name\":\"Wi-Fi Setup Portal (green long)\",\"group\":\"Front Panel\",\"builtin\":\"panelGreenLong\",\"triggers\":[{\"type\":\"panel\",\"button\":\"green\",\"gesture\":\"long\"}],\"actions\":[{\"type\":\"system\",\"command\":\"portal\"}]}",
    "{\"on\":true,\"name\":\"USB Mode Button (red)\",\"group\":\"Front Panel\",\"builtin\":\"panelRedPress\",\"triggers\":[{\"type\":\"panel\",\"button\":\"red\",\"gesture\":\"press\"}],\"actions\":[{\"type\":\"system\",\"command\":\"usbCycle\"}]}",
    "{\"on\":true,\"name\":\"USB Serial/JTAG (red long)\",\"group\":\"Front Panel\",\"builtin\":\"panelRedLong\",\"triggers\":[{\"type\":\"panel\",\"button\":\"red\",\"gesture\":\"long\"}],\"actions\":[{\"type\":\"system\",\"command\":\"usbJtag\"}]}"
};
}

bool actionRulesClaimTrs(const char* jack, const char* gesture) {
    return instance != nullptr && instance->claimsTrs(jack, gesture);
}

bool actionRulesClaimMidi(const char* kind, uint8_t channel, uint8_t number) {
    return instance != nullptr && instance->claimsMidi(kind, channel, number);
}

bool actionRulesClaimPanel(const char* button, const char* gesture) {
    return instance != nullptr && instance->claimsPanel(button, gesture);
}

void actionRulesOnMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value) {
    if (instance != nullptr) {
        instance->onMidi(kind, channel, number, value);
    }
}

void actionRulesOnPanel(const char* button, const char* gesture) {
    if (instance != nullptr) {
        instance->onPanel(button, gesture);
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

void ActionRules::begin(MultiMidi* midiIn, MackieShowbox* showboxIn, Connections* connectionsIn, Hid* hidIn) {
    midi = midiIn;
    showbox = showboxIn;
    connections = connectionsIn;
    hid = hidIn;
    instance = this;
    load();
    if (showbox != nullptr) {
        showbox->entityListener = entityHeard;
        showbox->packetListener = packetHeard;
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

void ActionRules::setFrontPanel(FrontPanel* panelIn) {
    panel = panelIn;
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
        if (varsDirty) {
            saveVars();
        }
        askDone = true;
    } else if (dirty) {
        flush();
    }
    if (varsDirty) {
        saveVars();
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
    if (link == "bleHost" || link == "ble") {
        return midi != nullptr && midi->hostConnected();
    }
    if (link == "blePeripheral") {
        return connections != nullptr && connections->bleIncomingOn();
    }
    if (link == "usbMidi" || link == "usbhost") {
        const char* usbStatus = UsbMidiHost::status();
        return usbStatus != nullptr && strstr(usbStatus, "device connected") != nullptr;
    }
    if (link == "din") {
        return midi != nullptr && midi->dinIsEnabled();
    }
    if (link == "apple") {
        return midi != nullptr && midi->wifiIsEnabled();
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

void ActionRules::noteMidiEcho(uint8_t cc, uint8_t channel) {
    ignoreMidiEchoCc = cc;
    ignoreMidiEchoCh = channel;
    ignoreMidiEchoUntil = millis() + kMidiEchoIgnoreMs;
}

bool ActionRules::isMidiEcho(uint8_t channel, uint8_t number) const {
    if (number != ignoreMidiEchoCc || channel != ignoreMidiEchoCh) {
        return false;
    }
    return (int32_t)(millis() - ignoreMidiEchoUntil) < 0;
}

bool ActionRules::wasRecentMidiCc(uint8_t cc, uint8_t channel) const {
    if (cc != lastMidiInCc || channel != lastMidiInCh) {
        return false;
    }
    return (int32_t)(millis() - lastMidiInUntil) < 0;
}

void ActionRules::onMidi(const char* kind, uint8_t channel, uint8_t number, uint8_t value) {
    if (kind != nullptr && strcmp(kind, "cc") == 0) {
        lastMidiInCc = number;
        lastMidiInCh = channel;
        lastMidiInUntil = millis() + kMidiEchoIgnoreMs;
        if (isMidiEcho(channel, number)) {
            return;
        }
    }
    if (inAction || (int32_t)(millis() - ignoreMidiUntil) < 0) {
        return;
    }
    char channelText[8];
    char numberText[8];
    char valueText[8];
    snprintf(channelText, sizeof(channelText), "%u", channel + 1);
    snprintf(numberText, sizeof(numberText), "%u", number);
    snprintf(valueText, sizeof(valueText), "%u", value);
    fire("midi", kind, channelText, numberText, valueText, midiInboundSource());
}

void ActionRules::onPanel(const char* button, const char* gesture) {
    fire("panel", button == nullptr ? "" : button, gesture == nullptr ? "" : gesture, "", "");
}

void ActionRules::onShowbox(const char* entity, const char* value) {
    if (inAction || (int32_t)(millis() - ignoreShowboxUntil) < 0) {
        return;
    }
    fire("showbox", entity, value == nullptr ? "" : value, "", "");
}

void ActionRules::onPacket(const uint8_t* data, size_t length, const char* dir) {
    if (inAction || data == nullptr || length == 0) {
        return;
    }
    if (length >= 6 && data[3] == ENTITY) {
        entity_id id = static_cast<entity_id>(data[5]);
        if (id == MAIN_HEADPHONE_GAIN || id == LOOPER_LEVEL) {
            return;
        }
    }
    char typeText[8] = "0";
    if (length >= 4) {
        snprintf(typeText, sizeof(typeText), "%u", data[3]);
    }
    String hex = toHex(data, length);
    fire("showboxPacket", dir == nullptr ? "" : dir, typeText, hex.c_str(), "");
}

bool ActionRules::claimsTrs(const char* jack, const char* gesture) const {
    if (jack == nullptr || gesture == nullptr) {
        return false;
    }
    for (const Rule& rule : rules) {
        if (!rule.on) {
            continue;
        }
        JsonDocument doc;
        if (deserializeJson(doc, rule.json)) {
            continue;
        }
        for (JsonObjectConst trigger : doc["triggers"].as<JsonArrayConst>()) {
            if (jsonText(trigger["type"]) == "trs" && jsonText(trigger["jack"]) == jack
                && jsonText(trigger["gesture"]) == gesture) {
                return true;
            }
        }
    }
    return false;
}

bool ActionRules::claimsMidi(const char* kind, uint8_t channel, uint8_t number) const {
    if (kind == nullptr) {
        return false;
    }
    for (const Rule& rule : rules) {
        if (!rule.on) {
            continue;
        }
        JsonDocument doc;
        if (deserializeJson(doc, rule.json)) {
            continue;
        }
        for (JsonObjectConst trigger : doc["triggers"].as<JsonArrayConst>()) {
            if (jsonText(trigger["type"]) != "midi") {
                continue;
            }
            String message = jsonText(trigger["message"]);
            if (!blank(message) && message != kind && !(message == "pitch" && String(kind) == "pitchBend")) {
                continue;
            }
            if (!numberOrAny(trigger["channel"], channel + 1)) {
                continue;
            }
            if (!numberOrAny(trigger["number"], number)) {
                continue;
            }
            return true;
        }
    }
    return false;
}

bool ActionRules::claimsPanel(const char* button, const char* gesture) const {
    if (button == nullptr || gesture == nullptr) {
        return false;
    }
    for (const Rule& rule : rules) {
        if (!rule.on) {
            continue;
        }
        JsonDocument doc;
        if (deserializeJson(doc, rule.json)) {
            continue;
        }
        for (JsonObjectConst trigger : doc["triggers"].as<JsonArrayConst>()) {
            if (jsonText(trigger["type"]) == "panel" && jsonText(trigger["button"]) == button
                && jsonText(trigger["gesture"]) == gesture) {
                return true;
            }
        }
    }
    return false;
}

bool ActionRules::triggerMatches(JsonObjectConst trigger, const char* event, const char* p1, const char* p2, const char* p3, const char* p4, const char* p5) const {
    String type = jsonText(trigger["type"]);
    if (type != event) {
        return false;
    }
    if (type == "trs" || type == "connect" || type == "panel") {
        String a = type == "connect" ? jsonText(trigger["link"])
            : (type == "panel" ? jsonText(trigger["button"]) : jsonText(trigger["jack"]));
        String b = type == "connect" ? jsonText(trigger["change"]) : jsonText(trigger["gesture"]);
        return a == p1 && b == p2;
    }
    if (type == "midi") {
        String message = jsonText(trigger["message"]);
        if (!blank(message) && message != p1 && !(message == "pitch" && String(p1) == "pitchBend")) {
            return false;
        }
        if (!midiPortNameMatches(trigger["ports"].isNull() ? trigger["port"] : trigger["ports"], p5)) {
            return false;
        }
        return numberOrAny(trigger["channel"], atoi(p2))
            && numberOrAny(trigger["number"], atoi(p3))
            && numberOrAny(trigger["value"], atoi(p4));
    }
    if (type == "showboxPacket") {
        if (!blank(jsonText(trigger["dir"])) && jsonText(trigger["dir"]) != p1 && jsonText(trigger["dir"]) != "any") {
            return false;
        }
        if (!numberOrAny(trigger["packetType"], atoi(p2)) && jsonText(trigger["packetType"]).length() > 0) {
            return false;
        }
        return hexPrefix(jsonText(trigger["hex"]), String(p3));
    }
    if (type == "variable") {
        if (jsonText(trigger["name"]) != p1) {
            return false;
        }
        String wanted = jsonText(trigger["value"]);
        if (blank(wanted)) {
            return true;
        }
        return compareNumber(wantedNumber(String(p2)), jsonText(trigger["op"]).length() ? jsonText(trigger["op"]) : "eq", wantedNumber(wanted));
    }
    if (type != "showbox") {
        return false;
    }
    if (jsonText(trigger["entity"]) != p1) {
        return false;
    }
    String wanted = jsonText(trigger["value"]);
    if (blank(wanted)) {
        return true;
    }
    if (wanted == p2) {
        return true;
    }
    char* endA = nullptr;
    char* endB = nullptr;
    float left = strtof(wanted.c_str(), &endA);
    float right = strtof(p2, &endB);
    return endA != wanted.c_str() && endB != p2 && fabsf(left - right) < 0.05f;
}

bool ActionRules::leafPass(JsonObjectConst leaf) const {
    String type = jsonText(leaf["type"]);
    if (type == "connection") {
        bool up = linkUp(jsonText(leaf["link"]));
        return (jsonText(leaf["state"]) == "up") == up;
    }
    if (type == "trs") {
        int index = jackIndex(jsonText(leaf["jack"]));
        if (index < 0) {
            return false;
        }
        bool pressed = digitalRead(kPins[index]) == LOW;
        String state = jsonText(leaf["state"]);
        bool wantPressed = state == "down" || state == "pressed";
        return pressed == wantPressed;
    }
    if (type == "midiPort") {
        return linkUp(jsonText(leaf["port"])) == (jsonText(leaf["state"]) != "down");
    }
    if (type == "variable") {
        String op = jsonText(leaf["op"]);
        if (op.length() == 0) {
            op = "eq";
        }
        return compareNumber(getVar(nullptr, jsonText(leaf["name"])), op, wantedNumber(jsonText(leaf["value"])));
    }
    if (type != "showbox") {
        return false;
    }
    float actual = 0;
    if (!showboxValue(showbox, jsonText(leaf["entity"]), actual)) {
        return false;
    }
    String op = jsonText(leaf["op"]);
    if (op.length() == 0) {
        op = "eq";
    }
    return compareNumber(actual, op, wantedNumber(jsonText(leaf["value"])));
}

bool ActionRules::conditionPass(JsonVariantConst node) const {
    if (node.isNull()) {
        return true;
    }
    if (node.is<JsonArrayConst>()) {
        for (JsonVariantConst item : node.as<JsonArrayConst>()) {
            if (!conditionPass(item)) {
                return false;
            }
        }
        return true;
    }
    if (!node.is<JsonObjectConst>()) {
        return true;
    }
    JsonObjectConst object = node.as<JsonObjectConst>();
    String op = jsonText(object["op"]);
    op.toLowerCase();
    if (op == "and" || op == "or" || op == "not") {
        JsonArrayConst items = object["items"].as<JsonArrayConst>();
        if (op == "not") {
            if (items.size() == 0) {
                return !conditionPass(object["condition"]);
            }
            return !conditionPass(items[0]);
        }
        bool any = false;
        bool saw = false;
        for (JsonVariantConst item : items) {
            saw = true;
            bool pass = conditionPass(item);
            if (op == "and" && !pass) {
                return false;
            }
            if (op == "or" && pass) {
                return true;
            }
            any = any || pass;
        }
        if (!saw) {
            return true;
        }
        return op == "and" ? true : any;
    }
    return leafPass(object);
}

void ActionRules::fire(const char* event, const char* p1, const char* p2, const char* p3, const char* p4, const char* p5) {
    for (const Rule& rule : rules) {
        if (!rule.on) {
            continue;
        }
        JsonDocument doc;
        if (deserializeJson(doc, rule.json)) {
            continue;
        }
        bool matched = false;
        for (JsonObjectConst trigger : doc["triggers"].as<JsonArrayConst>()) {
            if (triggerMatches(trigger, event, p1, p2, p3, p4, p5)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        JsonVariantConst condition = doc["condition"];
        if (condition.isNull()) {
            condition = doc["conditions"];
        }
        if (!conditionPass(condition)) {
            continue;
        }
        String looperOp;
        bool looperOnly = true;
        for (JsonObjectConst step : doc["actions"].as<JsonArrayConst>()) {
            String type = jsonText(step["type"]);
            if (type == "looper") {
                String button = jsonText(step["button"]);
                if (button.length() == 0) {
                    button = jsonText(step["op"]);
                }
                if (looperOp.length() == 0) {
                    looperOp = button;
                }
            } else {
                looperOnly = false;
            }
        }
        if (looperOp.length() && looperOnly) {
            dispatchLooper(looperOp);
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
    for (const Job& job : jobs) {
        if (job.active && job.id == id) {
            return false;
        }
    }
    Job job;
    job.active = true;
    job.id = id;
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
        JsonDocument doc;
        if (deserializeJson(doc, rules[slot].json)) {
            job.active = false;
            continue;
        }
        JsonArrayConst actions = doc["actions"].as<JsonArrayConst>();
        for (int burst = 0; burst < 64 && job.active; burst++) {
            if ((int32_t)(millis() - job.waitUntil) < 0) {
                break;
            }
            if (job.looperKind != 0) {
                runLooper(job);
                if (job.looperKind != 0) {
                    break;
                }
            }
            if (job.moveKind != 0) {
                uint32_t elapsed = millis() - job.moveStart;
                float t = job.moveDur == 0 ? 1.0f : (float)elapsed / (float)job.moveDur;
                if (t > 1) {
                    t = 1;
                }
                uint16_t x = (uint16_t)(job.x0 + (int)((int)job.x1 - (int)job.x0) * t);
                uint16_t y = (uint16_t)(job.y0 + (int)((int)job.y1 - (int)job.y0) * t);
                if (hid != nullptr) {
                    if (job.moveKind == 1 || job.moveKind == 3) {
                        hid->sendAbs(x, y, true, job.moveButtons);
                        job.lastAbsX = x;
                        job.lastAbsY = y;
                    } else {
                        int dx = (int)x - (int)job.lastMoveX;
                        int dy = (int)y - (int)job.lastMoveY;
                        int8_t sx = dx > 127 ? 127 : (dx < -127 ? -127 : (int8_t)dx);
                        int8_t sy = dy > 127 ? 127 : (dy < -127 ? -127 : (int8_t)dy);
                        hid->sendMouse(sx, sy, job.moveButtons, 0, 0, false);
                        job.lastMoveX = (int16_t)(job.lastMoveX + sx);
                        job.lastMoveY = (int16_t)(job.lastMoveY + sy);
                    }
                }
                if (t >= 1) {
                    if (hid != nullptr && (job.moveKind == 1 || job.moveKind == 3)) {
                        hid->sendAbs(job.x1, job.y1, job.moveButtons != 0, job.moveButtons);
                    }
                    job.moveKind = 0;
                } else {
                    job.waitUntil = millis() + 16;
                    break;
                }
            }
            if (job.step >= actions.size()) {
                job.active = false;
                break;
            }
            JsonObjectConst step = actions[job.step++];
            if (jsonText(step["type"]) == "sleep") {
                long ms = jsonInt(step["ms"], 0);
                if (ms < 0) {
                    ms = 0;
                }
                job.waitUntil = millis() + (uint32_t)ms;
                break;
            }
            if (!runStep(job, step)) {
                break;
            }
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

bool ActionRules::runStep(Job& job, JsonObjectConst step) {
    inAction = true;
    String type = jsonText(step["type"]);
    String error;
    if ((type == "midi" || type == "hidKey" || type == "hidMouse" || type == "hidScroll"
        || type == "hidTouch" || type == "hid") && type == "midi" && midi != nullptr) {
        String message = jsonText(step["message"]);
        int channel = jsonInt(step["channel"], 1);
        if (channel < 1) channel = 1;
        if (channel > 16) channel = 16;
        int8_t wire = (int8_t)(channel - 1);
        uint8_t number = (uint8_t)jsonInt(step["number"], 0);
        uint8_t value = (uint8_t)jsonInt(step["value"], 0);
        uint16_t ports = midiPortMask(step["ports"].isNull() ? step["port"] : step["ports"]);
        ignoreMidiUntil = millis() + 40;
        midi::MidiMessage msg = {};
        auto sendCh = [&](uint8_t statusHi, uint8_t a, uint8_t b, int len) {
            msg.status = statusHi | wire;
            msg.arg1 = a;
            msg.arg2 = b;
            midi->writeData(&msg, len, ports);
        };
        if (message == "noteOn") {
            sendCh(0x90, number, value, 2);
        } else if (message == "noteOff") {
            sendCh(0x80, number, value, 2);
        } else if (message == "pc") {
            sendCh(0xC0, number, 0, 1);
        } else if (message == "pitchBend" || message == "pitch") {
            uint16_t bend = (uint16_t)jsonInt(step["value"], 8192);
            sendCh(0xE0, bend & 0x7F, (bend >> 7) & 0x7F, 2);
        } else if (message == "aftertouch") {
            sendCh(0xD0, value, 0, 1);
        } else if (message == "polyAftertouch") {
            sendCh(0xA0, number, value, 2);
        } else if (message == "raw" || message == "sysex") {
            uint8_t bytes[128];
            size_t n = 0;
            if (parseHex(jsonText(step["hex"]), bytes, sizeof(bytes), n) && n > 0) {
                midi->writeBytes(bytes, n, ports);
            }
        } else if (message == "clock") {
            midi->writeRaw(0xF8, 0, 0, ports);
        } else if (message == "start") {
            midi->writeRaw(0xFA, 0, 0, ports);
        } else if (message == "continue") {
            midi->writeRaw(0xFB, 0, 0, ports);
        } else if (message == "stop") {
            midi->writeRaw(0xFC, 0, 0, ports);
        } else if (message == "reset") {
            midi->writeRaw(0xFF, 0, 0, ports);
        } else {
            sendCh(0xB0, number, value, 2);
        }
    } else if (type == "hid" || type == "hidKey" || type == "hidMouse" || type == "hidScroll" || type == "hidTouch") {
        if (hid == nullptr) {
            inAction = false;
            return true;
        }
        String device = jsonText(step["device"]);
        String op = jsonText(step["op"]);
        if (type == "hidKey") {
            device = "keyboard";
            op = "tap";
        } else if (type == "hidMouse") {
            device = "mouse";
            op = "move";
        } else if (type == "hidScroll") {
            device = "mouse";
            op = "scroll";
        } else if (type == "hidTouch") {
            device = "touch";
            op = jsonText(step["gesture"]);
            if (op.length() == 0) {
                op = "tap";
            }
        }
        if (device.length() == 0) {
            device = jsonText(step["key"]).length() ? "keyboard" : "mouse";
        }
        int x = jsonInt(step["x"], 0);
        int y = jsonInt(step["y"], 0);
        int x2 = jsonInt(step["x2"], x);
        int y2 = jsonInt(step["y2"], y);
        uint8_t buttons = mouseButtons(step);
        uint32_t speed = (uint32_t)jsonInt(step["speedMs"], jsonInt(step["speed"], 0));
        if (device == "keyboard") {
            if (op == "text") {
                if (connections != nullptr) {
                    connections->queueKeys(jsonText(step["text"]), error);
                }
            } else if (op == "media" || findMedia(jsonText(step["media"]).length() ? jsonText(step["media"]) : jsonText(step["key"]))) {
                const MediaMap* media = findMedia(jsonText(step["media"]).length() ? jsonText(step["media"]) : jsonText(step["key"]));
                if (media != nullptr && hid != nullptr) {
                    bool down = op != "up";
                    bool autoUp = op == "tap" || op == "click" || op.length() == 0 || op == "media";
                    hid->sendConsumer(media->usage, down, autoUp);
                }
            } else {
                const KeyMap* key = findKey(jsonText(step["key"]));
                uint8_t mods = modifiersOf(step["modifiers"]);
                if (key != nullptr && hid != nullptr) {
                    bool down = op != "up";
                    bool autoUp = op == "tap" || op == "click" || op.length() == 0;
                    hid->sendKey(key->usage, mods | key->modifier, down, autoUp);
                } else if (connections != nullptr && jsonText(step["key"]).length()) {
                    connections->queueKey(jsonText(step["key"]).c_str(), error);
                }
            }
        } else if (device == "mouse") {
            if (op == "down") {
                hid->sendMouse(0, 0, buttons ? buttons : 1, 0, 0, false);
            } else if (op == "up") {
                hid->sendMouse(0, 0, 0, 0, 0, false);
            } else if (op == "click" || op == "tap") {
                hid->sendMouse(0, 0, buttons ? buttons : 1, 0, 0, true);
            } else if (op == "double") {
                hid->sendMouse(0, 0, buttons ? buttons : 1, 0, 0, true);
                job.waitUntil = millis() + 40;
                job.step--;
                static uint8_t twice = 0;
                twice = (twice + 1) & 1;
                if (twice == 0) {
                    job.step++;
                }
            } else if (op == "scroll") {
                int amount = jsonInt(step["amount"], jsonInt(step["wheel"], 0));
                hid->sendMouse(0, 0, 0, (int8_t)(amount > 127 ? 127 : (amount < -127 ? -127 : amount)), 0, false);
            } else if (op == "move" && (speed > 0 || x2 != x || y2 != y)) {
                job.moveKind = 2;
                job.x0 = 0;
                job.y0 = 0;
                job.x1 = (uint16_t)((x2 != x || y2 != y) ? x2 : x);
                job.y1 = (uint16_t)((x2 != x || y2 != y) ? y2 : y);
                job.moveStart = millis();
                job.moveDur = speed == 0 ? 300 : speed;
                job.moveButtons = buttons;
                job.lastMoveX = 0;
                job.lastMoveY = 0;
            } else {
                int8_t dx = x > 127 ? 127 : (x < -127 ? -127 : (int8_t)x);
                int8_t dy = y > 127 ? 127 : (y < -127 ? -127 : (int8_t)y);
                hid->sendMouse(dx, dy, buttons, (int8_t)jsonInt(step["wheel"], 0), (int8_t)jsonInt(step["pan"], 0), op != "down");
            }
        } else {
            if (op == "down") {
                hid->sendAbs((uint16_t)x, (uint16_t)y, true, buttons ? buttons : 1);
            } else if (op == "up") {
                hid->sendAbs((uint16_t)x, (uint16_t)y, false, 0);
            } else if (op == "click" || op == "tap") {
                hid->sendTap((uint16_t)jsonInt(step["x"], 5000), (uint16_t)jsonInt(step["y"], 5000));
            } else if (op == "swipe") {
                hid->sendSwipe((uint16_t)jsonInt(step["x"], 5000), (uint16_t)jsonInt(step["y"], 5000),
                    (uint16_t)jsonInt(step["x2"], 7000), (uint16_t)jsonInt(step["y2"], 5000));
            } else if (op == "pinch-in" || op == "pinch") {
                hid->sendPinch((uint16_t)jsonInt(step["x"], 5000), (uint16_t)jsonInt(step["y"], 5000), true);
            } else if (op == "pinch-out") {
                hid->sendPinch((uint16_t)jsonInt(step["x"], 5000), (uint16_t)jsonInt(step["y"], 5000), false);
            } else if (op == "move") {
                job.moveKind = device == "touch" ? 3 : 1;
                job.x0 = (uint16_t)x;
                job.y0 = (uint16_t)y;
                job.x1 = (uint16_t)x2;
                job.y1 = (uint16_t)y2;
                job.moveStart = millis();
                job.moveDur = speed == 0 ? 300 : speed;
                job.moveButtons = buttons ? buttons : 1;
                hid->sendAbs(job.x0, job.y0, true, job.moveButtons);
            }
        }
    } else if (type == "hidScroll" && connections != nullptr) {
        connections->queueScroll(jsonInt(step["amount"], 0), error);
    } else if (type == "showbox" && showbox != nullptr) {
        auto found = string_to_entity_id.find(std::string(jsonText(step["entity"]).c_str()));
        if (found != string_to_entity_id.end()) {
            entity_id id = found->second;
            entity_data_type kind = entity_type_mapping[id];
            ignoreShowboxUntil = millis() + 80;
            String raw = jsonText(step["value"]);
            if (kind == BOOL) {
                raw.toLowerCase();
                showbox->setEntityValue(id, raw == "1" || raw == "true" || raw == "on");
            } else if (kind == UINT8) {
                showbox->setEntityValue(id, (uint8_t)raw.toInt());
            } else {
                showbox->setEntityValue(id, raw.length() == 0 ? 0.0f : raw.toFloat());
            }
        }
    } else if (type == "showboxPacket" && showbox != nullptr) {
        uint8_t bytes[280];
        size_t n = 0;
        if (parseHex(jsonText(step["hex"]), bytes, sizeof(bytes), n)) {
            showbox->injectPacket(bytes, n, jsonText(step["dir"]).c_str());
        }
    } else if (type == "looper" && showbox != nullptr) {
        String button = jsonText(step["button"]);
        if (button.length() == 0) {
            button = jsonText(step["op"]);
        }
        if (button == "cycle" || button == "stop" || button == "delete") {
            startLooper(job, button);
        } else {
            looper_button_action action = looper_button_action::DOWN;
            if (button == "up") action = looper_button_action::UP;
            else if (button == "double") action = looper_button_action::DOUBLE_PRESS;
            else if (button == "long") action = looper_button_action::LONG_PRESS;
            showbox->sendLooperButtonAction(action);
        }
    } else if (type == "gainBoost" && showbox != nullptr) {
        applyGainBoost(job, step);
    } else if (type == "variable") {
        String name = jsonText(step["name"]);
        String op = jsonText(step["op"]);
        if (op.length() == 0) {
            op = "set";
        }
        bool local = jsonText(step["scope"]) == "local";
        bool persist = !local;
        if (step["persist"].is<bool>()) {
            persist = !local && step["persist"].as<bool>();
        }
        float current = getVar(&job, name);
        float next = wantedNumber(jsonText(step["value"]));
        if (op == "toggle") {
            next = current >= 0.5f ? 0.0f : 1.0f;
        } else if (op == "add") {
            next = current + next;
        }
        setVar(&job, name, next, local, persist);
    } else if (type == "system") {
        runSystem(jsonText(step["command"]));
    } else if (type == "snapshot" && showbox != nullptr) {
        int slot = jsonInt(step["slot"], 1);
        if (slot >= 1 && slot <= 5) {
            showbox->snapshotAction(snapshot_action::RECALL, static_cast<snapshot_slot>(slot - 1));
            liveSocket.noteLink();
        }
    } else if (type == "sdcard" && showbox != nullptr) {
        showbox->toggleSdCardRecord();
        liveSocket.noteLink();
    } else if (type == "tuner" && showbox != nullptr) {
        tuner_chan chan = static_cast<tuner_chan>(jsonInt(step["chan"], 0));
        if (jsonText(step["action"]) == "off") {
            showbox->tunerAction(tuner_action::TURN_OFF, chan);
        } else {
            showbox->tunerAction(tuner_action::TURN_ON, chan);
        }
        liveSocket.noteLink();
    }
    inAction = false;
    if (debug != nullptr && error.length() > 0) {
        debug->printf("Reaction step skipped: %s\n", error.c_str());
    }
    return true;
}

String ActionRules::buildJson() const {
    JsonDocument doc;
    JsonArray list = doc["automations"].to<JsonArray>();
    for (const Rule& rule : rules) {
        JsonDocument one;
        if (deserializeJson(one, rule.json)) {
            continue;
        }
        list.add(one.as<JsonObject>());
    }
    String out;
    serializeJson(doc, out);
    return out;
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
    if (!object["triggers"].is<JsonArray>() || object["triggers"].as<JsonArray>().size() == 0) {
        error = "Add at least one trigger.";
        return false;
    }
    if (!object["actions"].is<JsonArray>() || object["actions"].as<JsonArray>().size() == 0) {
        error = "Add at least one action.";
        return false;
    }
    if (object["condition"].isNull() && object["conditions"].is<JsonArray>()) {
        JsonArray items = object["conditions"].as<JsonArray>();
        if (items.size() == 1) {
            object["condition"] = items[0];
        } else if (items.size() > 1) {
            JsonObject grouped = object["condition"].to<JsonObject>();
            grouped["op"] = "and";
            grouped["items"] = items;
        }
    }
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
    object["on"] = draft.on;
    object["name"] = draft.name;
    int slot = draft.id == 0 ? -1 : indexOf(draft.id);
    if (slot < 0) {
        if (draft.id == 0) {
            uint16_t next = 1;
            for (const Rule& rule : rules) {
                if (rule.id >= next) {
                    next = rule.id + 1;
                }
            }
            draft.id = next;
        }
        object["id"] = draft.id;
        serializeJson(object, draft.json);
        rules.push_back(draft);
    } else {
        draft.id = rules[slot].id;
        object["id"] = draft.id;
        serializeJson(object, draft.json);
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
    loadVars();
    Preferences prefs;
    if (prefs.begin("actions", true)) {
        String ids = prefs.getString("ids", "");
        prefs.end();
        const char* cursor = ids.c_str();
        while (*cursor != '\0') {
            char token[8];
            cursor = take(cursor, ',', token, sizeof(token));
            int id = atoi(token);
            if (id <= 0) {
                continue;
            }
            Preferences one;
            if (!one.begin("actions", true)) {
                continue;
            }
            String raw = one.getString(("a" + String(id)).c_str(), "");
            one.end();
            String error;
            if (raw.length() == 0 || !applySave(raw, error)) {
                continue;
            }
            storedIds.push_back((uint16_t)id);
        }
    }
    dirty = false;
    seedBuiltins();
    if (dirty) {
        flush();
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
        String key = "a" + String(rules[i].id);
        if (prefs.putString(key.c_str(), rules[i].json) == 0 && debug != nullptr) {
            debug->println("Could not store a reaction.");
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

bool ActionRules::hasBuiltin(const char* key) const {
    if (key == nullptr || key[0] == '\0') {
        return true;
    }
    for (const Rule& rule : rules) {
        JsonDocument doc;
        if (deserializeJson(doc, rule.json)) {
            continue;
        }
        if (jsonText(doc["builtin"]) == key) {
            return true;
        }
    }
    return false;
}

void ActionRules::seedBuiltins() {
    for (const char* raw : kBuiltinRules) {
        JsonDocument doc;
        if (deserializeJson(doc, raw)) {
            continue;
        }
        String key = jsonText(doc["builtin"]);
        if (hasBuiltin(key.c_str())) {
            continue;
        }
        doc.remove("id");
        String body;
        serializeJson(doc, body);
        String error;
        applySave(body, error);
    }
    patchBuiltinMeta();
}

void ActionRules::patchBuiltinMeta() {
    for (const char* raw : kBuiltinRules) {
        JsonDocument seed;
        if (deserializeJson(seed, raw)) {
            continue;
        }
        String key = jsonText(seed["builtin"]);
        String name = jsonText(seed["name"]);
        String group = jsonText(seed["group"]);
        if (key.length() == 0) {
            continue;
        }
        for (Rule& rule : rules) {
            JsonDocument doc;
            if (deserializeJson(doc, rule.json)) {
                continue;
            }
            if (jsonText(doc["builtin"]) != key) {
                continue;
            }
            bool changed = false;
            if (name.length() > 0 && jsonText(doc["name"]) != name) {
                doc["name"] = name;
                rule.name = name;
                changed = true;
            }
            if (group.length() > 0 && jsonText(doc["group"]) != group) {
                doc["group"] = group;
                changed = true;
            }
            // MIDI 105/106 already arrived from a controller. Echoing them back
            // out kMidiPortAll loops BLE/ETH/ext forever.
            if (key == "midiBoostOn" || key == "midiBoostOff" || key == "midiBoostToggle"
                || key == "autoBoostOn" || key == "autoBoostOff" || key == "autoBoostArmed"
                || key == "autoBoostDisarmed") {
                for (JsonObject action : doc["actions"].as<JsonArray>()) {
                    if (jsonText(action["type"]) != "gainBoost") {
                        continue;
                    }
                    if (key == "midiBoostOn" || key == "midiBoostOff" || key == "midiBoostToggle") {
                        if (!action["echoCc"].isNull() || !action["echoChannel"].isNull()) {
                            action.remove("echoCc");
                            action.remove("echoChannel");
                            changed = true;
                        }
                    }
                    if (jsonInt(action["steps"], 4) != 2) {
                        action["steps"] = 2;
                        changed = true;
                    }
                }
            }
            if (key == "autoBoostArmed" || key == "autoBoostDisarmed") {
                if (doc["on"] != false) { doc["on"] = false; changed = true; }
            }
            if (key == "autoBoostOn" || key == "autoBoostOff") {
                String have;
                String want;
                serializeJson(doc["condition"], have);
                serializeJson(seed["condition"], want);
                if (have != want) {
                    doc["condition"] = seed["condition"];
                    changed = true;
                }
            }
            if (changed) {
                String body;
                serializeJson(doc, body);
                rule.json = body;
                dirty = true;
            }
        }
    }
}

void ActionRules::loadVars() {
    globalCount = 0;
    Preferences prefs;
    if (!prefs.begin("actvar", true)) {
        return;
    }
    String blob = prefs.getString("blob", "");
    prefs.end();
    if (blob.length() == 0) {
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, blob) || !doc.is<JsonObject>()) {
        return;
    }
    for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
        if (globalCount >= 24) {
            break;
        }
        copyVarName(globals[globalCount].name, sizeof(globals[globalCount].name), String(pair.key().c_str()));
        if (globals[globalCount].name[0] == '\0') {
            continue;
        }
        globals[globalCount].value = pair.value().as<float>();
        globals[globalCount].persist = true;
        globalCount++;
    }
}

void ActionRules::saveVars() {
    JsonDocument doc;
    JsonObject object = doc.to<JsonObject>();
    for (uint8_t i = 0; i < globalCount; i++) {
        if (globals[i].persist && globals[i].name[0] != '\0') {
            object[globals[i].name] = globals[i].value;
        }
    }
    String blob;
    serializeJson(object, blob);
    Preferences prefs;
    if (prefs.begin("actvar", false)) {
        prefs.putString("blob", blob);
        prefs.end();
    }
    varsDirty = false;
}

float ActionRules::getVar(Job* job, const String& name) const {
    char key[20];
    copyVarName(key, sizeof(key), name);
    if (key[0] == '\0') {
        return 0;
    }
    if (job != nullptr) {
        for (uint8_t i = 0; i < job->localCount; i++) {
            if (strcmp(job->locals[i].name, key) == 0) {
                return job->locals[i].value;
            }
        }
    }
    for (uint8_t i = 0; i < globalCount; i++) {
        if (strcmp(globals[i].name, key) == 0) {
            return globals[i].value;
        }
    }
    return 0;
}

void ActionRules::setVar(Job* job, const String& name, float value, bool local, bool persist) {
    char key[20];
    copyVarName(key, sizeof(key), name);
    if (key[0] == '\0') {
        return;
    }
    if (local && job != nullptr) {
        for (uint8_t i = 0; i < job->localCount; i++) {
            if (strcmp(job->locals[i].name, key) == 0) {
                job->locals[i].value = value;
                return;
            }
        }
        if (job->localCount < 8) {
            strncpy(job->locals[job->localCount].name, key, sizeof(job->locals[0].name) - 1);
            job->locals[job->localCount].name[sizeof(job->locals[0].name) - 1] = '\0';
            job->locals[job->localCount].value = value;
            job->localCount++;
        }
        return;
    }
    float previous = getVar(nullptr, name);
    int slot = -1;
    for (uint8_t i = 0; i < globalCount; i++) {
        if (strcmp(globals[i].name, key) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (globalCount >= 24) {
            return;
        }
        slot = globalCount++;
        strncpy(globals[slot].name, key, sizeof(globals[slot].name) - 1);
        globals[slot].name[sizeof(globals[slot].name) - 1] = '\0';
    }
    globals[slot].value = value;
    if (persist) {
        globals[slot].persist = true;
        varsDirty = true;
    }
    if (fabsf(previous - value) >= 0.0001f) {
        char text[24];
        snprintf(text, sizeof(text), "%g", value);
        fire("variable", key, text, "", "");
    }
}

void ActionRules::applyGainBoost(Job& job, JsonObjectConst step) {
    String entity = jsonText(step["entity"]);
    if (entity.length() == 0) {
        entity = "INPUT2_GAIN";
    }
    auto found = string_to_entity_id.find(std::string(entity.c_str()));
    if (found == string_to_entity_id.end()) {
        return;
    }
    entity_id id = found->second;
    float current = showbox->getFloatEntityValue(id);
    String mode = jsonText(step["on"]);
    if (mode.length() == 0) {
        mode = "toggle";
    }
    String varName = jsonText(step["var"]);
    bool persist = true;
    if (step["persist"].is<bool>()) {
        persist = step["persist"].as<bool>();
    }
    bool haveRange = !step["low"].isNull() || !step["high"].isNull();
    float low = step["low"].isNull() ? Showbox::InputGain::POSITION_9 : step["low"].as<float>();
    float high = step["high"].isNull() ? Showbox::InputGain::POSITION_14 : step["high"].as<float>();
    bool on = false;
    bool changed = false;
    if (haveRange) {
        bool atHigh = fabsf(current - high) < fabsf(current - low);
        if (mode == "on") on = true;
        else if (mode == "off") on = false;
        else on = !atHigh;
        float next = on ? high : low;
        if (fabsf(current - next) >= 0.05f) {
            ignoreShowboxUntil = millis() + 80;
            showbox->setEntityValue(id, next);
            changed = true;
        }
        if (varName.length()) {
            setVar(&job, varName, on ? 1.0f : 0.0f, false, persist);
        }
    } else {
        int steps = jsonInt(step["steps"], 2);
        if (steps < 1) steps = 1;
        String baseName = varName.length() ? varName + "Base" : String("boostBase");
        bool active = getVar(&job, varName.length() ? varName : String("boostOn")) >= 0.5f;
        if (mode == "on") on = true;
        else if (mode == "off") on = false;
        else on = !active;
        if (on != active) {
            changed = true;
            if (on) {
                int stepIndex = nearestGainStep(current) + steps;
                if (stepIndex >= kGainStepCount) {
                    stepIndex = kGainStepCount - 1;
                }
                ignoreShowboxUntil = millis() + 80;
                showbox->setEntityValue(id, kGainSteps[stepIndex]);
                setVar(&job, baseName, current, false, persist);
                if (varName.length()) {
                    setVar(&job, varName, 1.0f, false, persist);
                }
            } else {
                float base = getVar(&job, baseName);
                int boosted = nearestGainStep(current);
                int expected = nearestGainStep(base) + steps;
                if (expected >= kGainStepCount) expected = kGainStepCount - 1;
                ignoreShowboxUntil = millis() + 80;
                if (boosted == expected) {
                    showbox->setEntityValue(id, base);
                }
                if (varName.length()) {
                    setVar(&job, varName, 0.0f, false, persist);
                }
            }
        }
    }
    int echoCc = jsonInt(step["echoCc"], -1);
    if (changed && echoCc >= 0 && midi != nullptr) {
        int channel = jsonInt(step["echoChannel"], 16);
        if (channel < 1) channel = 1;
        if (channel > 16) channel = 16;
        uint8_t wire = (uint8_t)(channel - 1);
        // Do not rebroadcast a CC we just received; BLE/ETH/ext loop it back.
        if (wasRecentMidiCc((uint8_t)echoCc, wire)) {
            return;
        }
        midi::MidiMessage msg = {};
        msg.status = 0xB0 | wire;
        msg.arg1 = (uint8_t)echoCc;
        msg.arg2 = 127;
        noteMidiEcho((uint8_t)echoCc, wire);
        midi->writeData(&msg, 2);
    }
}

void ActionRules::dispatchLooper(const String& op) {
    if (showbox == nullptr) {
        return;
    }
    const uint8_t state = showbox->getUint8EntityValue(entity_id::LOOPER_STATE);
    const bool alreadySent = showbox->takeLooperOutgoing();
    auto sendTap = [&](uint8_t times) {
        if (!alreadySent) {
            showbox->tapLooper(times);
        }
    };
    auto sendAction = [&](looper_button_action action) {
        if (!alreadySent) {
            showbox->sendLooperButtonAction(action);
        }
    };

    if (op == "stop") {
        sendAction(looper_button_action::DOUBLE_PRESS);
        applyMidiLoopBoost(false);
        noteLooperState((state == looper_state::RECORD_INITIAL_LOOP || state == looper_state::DELETE)
            ? looper_state::DELETE : looper_state::STOP_PLAYING);
        return;
    }
    if (op == "delete") {
        sendAction(looper_button_action::LONG_PRESS);
        applyMidiLoopBoost(false);
        noteLooperState(looper_state::DELETE);
        return;
    }
    if (op == "up") {
        sendAction(looper_button_action::UP);
        return;
    }
    if (op == "double") {
        sendAction(looper_button_action::DOUBLE_PRESS);
        return;
    }
    if (op == "long") {
        sendAction(looper_button_action::LONG_PRESS);
        return;
    }
    // cycle / down / tap. UART first, then boost/state. Rec -> overdub is
    // two taps back-to-back (DOWN/UP/DOWN/UP) with no software gap.
    if (state == looper_state::RECORD_INITIAL_LOOP) {
        sendTap(2);
        applyMidiLoopBoost(false);
        noteLooperState(looper_state::RECORD_OVERDUB);
        ignoreShowboxUntil = millis() + 450;
        return;
    }
    if (state == looper_state::PLAY) {
        sendTap(1);
        applyMidiLoopBoost(false);
        noteLooperState(looper_state::RECORD_OVERDUB);
        return;
    }
    if (state == looper_state::RECORD_OVERDUB) {
        sendTap(1);
        applyMidiLoopBoost(true);
        noteLooperState(looper_state::PLAY);
        return;
    }
    if (state == looper_state::STOP_PLAYING) {
        sendTap(1);
        applyMidiLoopBoost(false);
        noteLooperState(looper_state::PLAY);
        return;
    }
    sendTap(1);
    applyMidiLoopBoost(false);
    noteLooperState(looper_state::RECORD_INITIAL_LOOP);
}

void ActionRules::noteLooperState(uint8_t next) {
    if (showbox == nullptr) {
        return;
    }
    uint8_t now = showbox->getUint8EntityValue(entity_id::LOOPER_STATE);
    if (next == now) {
        return;
    }
    showbox->setLooperState(static_cast<looper_state>(next));
    liveSocket.pushEntity("LOOPER_STATE", next);
}

void ActionRules::applyMidiLoopBoost(bool on) {
    if (on && getVar(nullptr, "autoBoostOff") >= 0.5f) {
        return;
    }
    Job job;
    JsonDocument doc;
    JsonObject step = doc.to<JsonObject>();
    step["entity"] = "INPUT2_GAIN";
    step["on"] = on ? "on" : "off";
    step["steps"] = 2;
    step["var"] = "midiBoost";
    step["persist"] = true;
    step["echoCc"] = on ? 105 : 106;
    step["echoChannel"] = 16;
    applyGainBoost(job, step);
}

void ActionRules::startLooper(Job& job, const String& op) {
    dispatchLooper(op);
    job.looperKind = 0;
}

void ActionRules::runLooper(Job& job) {
    job.looperKind = 0;
}

void ActionRules::runSystem(const String& command) {
    if (command == "bluetoothToggle" && connections != nullptr) {
        connections->setBluetoothRadio(!connections->bluetoothRadioOn());
        return;
    }
    if (command == "bluetoothOn" && connections != nullptr) {
        if (!connections->bluetoothRadioOn()) {
            connections->setBluetoothRadio(true);
        }
        return;
    }
    if (command == "bluetoothOff" && connections != nullptr) {
        if (connections->bluetoothRadioOn()) {
            connections->setBluetoothRadio(false);
        }
        return;
    }
    if (command == "pairing" && connections != nullptr) {
        if (!connections->bluetoothRadioOn()) {
            connections->setBluetoothRadio(true);
            return;
        }
        if (connections->pairingWindowOpen()) {
            connections->closePairingWindow();
        } else {
            connections->openPairingWindow(30000);
        }
        return;
    }
    if ((command == "wifiToggle" || command == "wifiOn" || command == "wifiOff" || command == "portal") && panel != nullptr) {
        bool on = panel->wifiRadioOn();
        if (command == "wifiToggle") {
            panel->applyWifi(!on);
        } else if (command == "wifiOn") {
            if (!on) panel->applyWifi(true);
        } else if (command == "wifiOff") {
            if (on) panel->applyWifi(false);
        } else {
            if (!on) {
                panel->applyWifi(true);
            }
            panel->startWifiPortal();
        }
        return;
    }
    if (connections == nullptr) {
        return;
    }
    if (command == "usbCycle") {
        Connections::UsbPortMode next = Connections::UsbPortMode::Host;
        if (connections->usbHostOn()) {
            next = Connections::UsbPortMode::Peripheral;
        } else if (connections->usbDeviceOn() || connections->usbHidOn()) {
            next = Connections::UsbPortMode::Jtag;
        }
        connections->setUsbPortMode(next);
        return;
    }
    if (command == "usbHost") {
        connections->setUsbPortMode(Connections::UsbPortMode::Host);
        return;
    }
    if (command == "usbPeripheral") {
        connections->setUsbPortMode(Connections::UsbPortMode::Peripheral);
        return;
    }
    if (command == "usbJtag") {
        connections->setUsbPortMode(Connections::UsbPortMode::Jtag);
    }
}
