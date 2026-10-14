#include "TwoButtonLooper.h"
#include "Actions/ActionRules.h"
#define SHOWBOX_DEBUG 1

TwoButtonLooper::TwoButtonLooper(uint8_t record_button_pin, uint8_t stop_button_pin, MackieShowbox* showbox)
    : recordButton(record_button_pin, true), stopButton(stop_button_pin, true), showbox(showbox) {}

void TwoButtonLooper::begin() {
    recordButton.attachClick(recordOverdubPlayCallback, this);
    stopButton.attachClick(stopCallback, this);
    stopButton.attachLongPressStart(deleteCallback, this);
    uint8_t state = showbox->getUint8EntityValue(entity_id::LOOPER_STATE);
    if (state == RECORD_INITIAL_LOOP) {
        mode = Mode::Recording;
    } else if (state == RECORD_OVERDUB) {
        mode = Mode::Overdub;
    } else if (state == PLAY) {
        mode = Mode::Playing;
    } else if (state == STOP_PLAYING) {
        mode = Mode::Stopped;
    } else {
        mode = Mode::Empty;
    }
}

void TwoButtonLooper::tick() {
    runJob();
    recordButton.tick();
    stopButton.tick();
}

void TwoButtonLooper::recordOverdubPlayCallback(void *instance) {
    TwoButtonLooper *looper = static_cast<TwoButtonLooper*>(instance);
    looper->handleRecordOverdubPlay();
}

void TwoButtonLooper::stopCallback(void *instance) {
    TwoButtonLooper *looper = static_cast<TwoButtonLooper*>(instance);
    looper->handleStop();
}

void TwoButtonLooper::deleteCallback(void *instance) {
    TwoButtonLooper *looper = static_cast<TwoButtonLooper*>(instance);
    looper->handleDelete();
}

void TwoButtonLooper::queueJob(Job next) {
    job = next;
    jobStep = 0;
    jobAt = millis();
}

void TwoButtonLooper::sendButton(looper_button_action action) {
    showbox->sendLooperButtonAction(action);
}

void TwoButtonLooper::runJob() {
    if (job == Job::None) {
        return;
    }
    if ((int32_t)(millis() - jobAt) < 0) {
        return;
    }
    if (job == Job::Tap) {
        if (jobStep == 0) {
            sendButton(looper_button_action::DOWN);
            jobStep = 1;
            jobAt = millis() + kTapGapMs;
            return;
        }
        sendButton(looper_button_action::UP);
        job = Job::None;
        return;
    }
    if (job == Job::Stop) {
        sendButton(looper_button_action::DOUBLE_PRESS);
        job = Job::None;
        Debug->println("Stop playing");
        return;
    }
    if (job == Job::Delete) {
        sendButton(looper_button_action::LONG_PRESS);
        job = Job::None;
        Debug->println("Set LOOPER_STATE to DELETE");
    }
}

void TwoButtonLooper::handleRecordOverdubPlay() {
    if (actionRulesClaimTrs("1t", "shortRelease")) {
        return;
    }
    if (job != Job::None) {
        return;
    }
    sendButton(looper_button_action::DOWN);
    sendButton(looper_button_action::UP);
    switch (mode) {
        case Mode::Empty:
            mode = Mode::Recording;
            Debug->println("Recording initial loop");
            break;
        case Mode::Recording:
            mode = Mode::Playing;
            Debug->println("Play");
            break;
        case Mode::Overdub:
            mode = Mode::Playing;
            Debug->println("Play");
            break;
        case Mode::Playing:
            mode = Mode::Overdub;
            Debug->println("Recording overdub");
            break;
        case Mode::Stopped:
            mode = Mode::Playing;
            Debug->println("Play");
            break;
    }
}

void TwoButtonLooper::handleStop() {
    if (actionRulesClaimTrs("1r", "shortRelease")) {
        return;
    }
    if (mode == Mode::Empty) {
        return;
    }
    mode = (mode == Mode::Recording) ? Mode::Empty : Mode::Stopped;
    sendButton(looper_button_action::DOUBLE_PRESS);
}

void TwoButtonLooper::handleDelete() {
    if (actionRulesClaimTrs("1r", "long")) {
        return;
    }
    mode = Mode::Empty;
    sendButton(looper_button_action::LONG_PRESS);
}

void TwoButtonLooper::setDebugSerial(Print* serial) {
    Debug = serial;
}
