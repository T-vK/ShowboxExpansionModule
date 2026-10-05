#include "BlePeers.h"
#include <Preferences.h>
#include <cstring>

namespace {
constexpr char kSep = '\x1f';

String field(const String& raw, int index) {
    int start = 0;
    int current = 0;
    for (int i = 0; i <= raw.length(); i++) {
        if (i == raw.length() || raw.charAt(i) == kSep) {
            if (current == index) {
                return raw.substring(start, i);
            }
            current++;
            start = i + 1;
        }
    }
    return "";
}
}

void BlePeers::load() {
    Preferences prefs;
    if (!prefs.begin("blepeers", true)) {
        return;
    }
    int stored = prefs.getInt("n", 0);
    if (stored > kMax) {
        stored = kMax;
    }
    if (stored < 0) {
        stored = 0;
    }
    portENTER_CRITICAL(&mux);
    count = 0;
    for (int i = 0; i < stored; i++) {
        String raw = prefs.getString((String("p") + i).c_str(), "");
        String address = field(raw, 0);
        if (address.length() < 11) {
            continue;
        }
        Peer& peer = peers[count++];
        memset(&peer, 0, sizeof(peer));
        strncpy(peer.address, address.c_str(), sizeof(peer.address) - 1);
        String name = field(raw, 1);
        strncpy(peer.name, name.c_str(), sizeof(peer.name) - 1);
        int flags = field(raw, 2).toInt();
        peer.midi = flags & 1;
        peer.hid = flags & 2;
        peer.autoConnect = flags & 4;
        peer.serialMidi = flags & 8;
        peer.host = flags & 16;
        peer.hidKeyboard = flags & 32;
        peer.hidMouse = flags & 64;
        peer.hidAbs = flags & 128;
        peer.hidDigitizer = flags & 256;
        peer.hidMixStored = flags & 512;
        peer.advertisedHid = flags & 1024;
        peer.advertisedBattery = flags & 2048;
        peer.pinned = flags & 4096;
    }
    portEXIT_CRITICAL(&mux);
    prefs.end();
}

void BlePeers::save() const {
    dirty = true;
}

void BlePeers::persist() const {
    if (!dirty) {
        return;
    }
    Preferences prefs;
    if (!prefs.begin("blepeers", false)) {
        return;
    }
    prefs.putInt("n", count);
    for (int i = 0; i < count; i++) {
        int flags = (peers[i].midi ? 1 : 0) | (peers[i].hid ? 2 : 0) | (peers[i].autoConnect ? 4 : 0) |
            (peers[i].serialMidi ? 8 : 0) | (peers[i].host ? 16 : 0) |
            (peers[i].hidKeyboard ? 32 : 0) | (peers[i].hidMouse ? 64 : 0) |
            (peers[i].hidAbs ? 128 : 0) | (peers[i].hidDigitizer ? 256 : 0) |
            (peers[i].hidMixStored ? 512 : 0) | (peers[i].advertisedHid ? 1024 : 0) |
            (peers[i].advertisedBattery ? 2048 : 0) | (peers[i].pinned ? 4096 : 0);
        String name = peers[i].name;
        name.replace(String(kSep), " ");
        String raw = String(peers[i].address) + kSep + name + kSep + String(flags);
        prefs.putString((String("p") + i).c_str(), raw);
    }
    prefs.end();
    dirty = false;
}

int BlePeers::indexOf(const char* address) const {
    if (address == nullptr) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (strcasecmp(peers[i].address, address) == 0) {
            return i;
        }
    }
    return -1;
}

int BlePeers::copy(Peer* out, int max) const {
    if (out == nullptr || max <= 0) {
        return 0;
    }
    portENTER_CRITICAL(&mux);
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) {
        out[i] = peers[i];
    }
    portEXIT_CRITICAL(&mux);
    return n;
}

bool BlePeers::find(const char* address, Peer* out) const {
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    if (slot >= 0 && out != nullptr) {
        *out = peers[slot];
    }
    portEXIT_CRITICAL(&mux);
    return slot >= 0;
}

bool BlePeers::findMatch(const char* a, const char* b, Peer* out) const {
    portENTER_CRITICAL(&mux);
    int slot = indexOf(a);
    if (slot < 0) {
        slot = indexOf(b);
    }
    if (slot >= 0 && out != nullptr) {
        *out = peers[slot];
    }
    portEXIT_CRITICAL(&mux);
    return slot >= 0;
}

bool BlePeers::rekey(const char* from, const char* to) {
    if (from == nullptr || to == nullptr || strlen(to) < 11) {
        return false;
    }
    if (strcasecmp(from, to) == 0) {
        return true;
    }
    portENTER_CRITICAL(&mux);
    int fromSlot = indexOf(from);
    if (fromSlot < 0) {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    int toSlot = indexOf(to);
    if (toSlot < 0) {
        strncpy(peers[fromSlot].address, to, sizeof(peers[fromSlot].address) - 1);
        peers[fromSlot].address[sizeof(peers[fromSlot].address) - 1] = '\0';
        portEXIT_CRITICAL(&mux);
        save();
        return true;
    }
    Peer keep = peers[toSlot];
    Peer drop = peers[fromSlot];
    if (keep.name[0] == '\0' && drop.name[0] != '\0') {
        strncpy(keep.name, drop.name, sizeof(keep.name) - 1);
        keep.name[sizeof(keep.name) - 1] = '\0';
    }
    keep.midi = keep.midi || drop.midi;
    keep.hid = keep.hid || drop.hid;
    keep.host = keep.host || drop.host;
    keep.autoConnect = keep.autoConnect || drop.autoConnect;
    if (drop.hidMixStored && !keep.hidMixStored) {
        keep.hidKeyboard = drop.hidKeyboard;
        keep.hidMouse = drop.hidMouse;
        keep.hidAbs = drop.hidAbs;
        keep.hidDigitizer = drop.hidDigitizer;
        keep.hidMixStored = true;
    }
    keep.advertisedHid = keep.advertisedHid || drop.advertisedHid;
    keep.advertisedBattery = keep.advertisedBattery || drop.advertisedBattery;
    keep.pinned = keep.pinned || drop.pinned;
    peers[toSlot] = keep;
    for (int i = fromSlot; i < count - 1; i++) {
        peers[i] = peers[i + 1];
    }
    count--;
    memset(&peers[count], 0, sizeof(peers[count]));
    portEXIT_CRITICAL(&mux);
    save();
    return true;
}

bool BlePeers::upsert(const char* address, const char* name, int midi, int hid, int autoConnect, int serialMidi, int host) {
    if (address == nullptr || strlen(address) < 11) {
        return false;
    }
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    if (slot < 0) {
        if (count >= kMax) {
            portEXIT_CRITICAL(&mux);
            return false;
        }
        slot = count++;
        memset(&peers[slot], 0, sizeof(peers[slot]));
        strncpy(peers[slot].address, address, sizeof(peers[slot].address) - 1);
    }
    if (name != nullptr && name[0] != '\0') {
        strncpy(peers[slot].name, name, sizeof(peers[slot].name) - 1);
        peers[slot].name[sizeof(peers[slot].name) - 1] = '\0';
    }
    if (midi >= 0) {
        peers[slot].midi = midi != 0;
    }
    if (hid >= 0) {
        peers[slot].hid = hid != 0;
    }
    if (autoConnect >= 0) {
        peers[slot].autoConnect = autoConnect != 0;
    }
    if (serialMidi >= 0) {
        peers[slot].serialMidi = serialMidi != 0;
    }
    if (host >= 0) {
        peers[slot].host = host != 0;
    }
    portEXIT_CRITICAL(&mux);
    save();
    return true;
}

bool BlePeers::setHidMix(const char* address, bool keyboard, bool mouse, bool absPointer, bool digitizer) {
    if (!keyboard && !mouse && !absPointer && !digitizer) {
        return false;
    }
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    if (slot < 0) {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    peers[slot].hid = true;
    peers[slot].hidKeyboard = keyboard;
    peers[slot].hidMouse = mouse;
    peers[slot].hidAbs = absPointer;
    peers[slot].hidDigitizer = digitizer;
    peers[slot].hidMixStored = true;
    portEXIT_CRITICAL(&mux);
    save();
    return true;
}

bool BlePeers::setPinned(const char* address, bool pinned) {
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    if (slot < 0) {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    peers[slot].pinned = pinned;
    portEXIT_CRITICAL(&mux);
    save();
    return true;
}

bool BlePeers::noteAdvertised(const char* address, bool midi, bool hid, bool battery) {
    if (address == nullptr || strlen(address) < 11) {
        return false;
    }
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    if (slot < 0 || peers[slot].host) {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    bool changed = false;
    if (midi && !peers[slot].midi) {
        peers[slot].midi = true;
        changed = true;
    }
    if (hid && !peers[slot].advertisedHid) {
        peers[slot].advertisedHid = true;
        changed = true;
    }
    if (battery && !peers[slot].advertisedBattery) {
        peers[slot].advertisedBattery = true;
        changed = true;
    }
    portEXIT_CRITICAL(&mux);
    if (changed) {
        save();
    }
    return true;
}

bool BlePeers::forget(const char* address) {
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    if (slot < 0) {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    for (int i = slot; i < count - 1; i++) {
        peers[i] = peers[i + 1];
    }
    count--;
    memset(&peers[count], 0, sizeof(peers[count]));
    portEXIT_CRITICAL(&mux);
    save();
    return true;
}

int BlePeers::midiPeerCount() const {
    portENTER_CRITICAL(&mux);
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (peers[i].midi && !peers[i].host) {
            n++;
        }
    }
    portEXIT_CRITICAL(&mux);
    return n;
}

int BlePeers::pinnedCount(bool hosts) const {
    portENTER_CRITICAL(&mux);
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (peers[i].pinned && peers[i].host == hosts) {
            n++;
        }
    }
    portEXIT_CRITICAL(&mux);
    return n;
}

bool BlePeers::matchesMidiAuto(const char* address) const {
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    bool match = slot >= 0 && peers[slot].midi && peers[slot].autoConnect && !peers[slot].host;
    portEXIT_CRITICAL(&mux);
    return match;
}
