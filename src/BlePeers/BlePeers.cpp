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
    }
    portEXIT_CRITICAL(&mux);
    prefs.end();
}

void BlePeers::save() const {
    Preferences prefs;
    if (!prefs.begin("blepeers", false)) {
        return;
    }
    prefs.putInt("n", count);
    for (int i = 0; i < count; i++) {
        int flags = (peers[i].midi ? 1 : 0) | (peers[i].hid ? 2 : 0) | (peers[i].autoConnect ? 4 : 0) |
            (peers[i].serialMidi ? 8 : 0) | (peers[i].host ? 16 : 0);
        String name = peers[i].name;
        name.replace(String(kSep), " ");
        String raw = String(peers[i].address) + kSep + name + kSep + String(flags);
        prefs.putString((String("p") + i).c_str(), raw);
    }
    prefs.end();
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

bool BlePeers::matchesMidiAuto(const char* address) const {
    portENTER_CRITICAL(&mux);
    int slot = indexOf(address);
    bool match = slot >= 0 && peers[slot].midi && peers[slot].autoConnect && !peers[slot].host;
    portEXIT_CRITICAL(&mux);
    return match;
}
