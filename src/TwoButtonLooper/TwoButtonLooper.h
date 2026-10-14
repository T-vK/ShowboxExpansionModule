#ifndef TWO_BUTTON_LOOPER_H
#define TWO_BUTTON_LOOPER_H

#include <Arduino.h>
#include <OneButton.h>
#include "MackieShowbox/MackieShowbox.h"

class TwoButtonLooper {
public:
    TwoButtonLooper(uint8_t record_button_pin, uint8_t stop_button_pin, MackieShowbox* showbox);
    void begin();
    void tick();
    void setDebugSerial(Print* serial);

private:
    enum class Mode : uint8_t {
        Empty,
        Recording,
        Overdub,
        Playing,
        Stopped
    };
    enum class Job : uint8_t {
        None,
        Tap,
        Stop,
        Delete
    };

    static void recordOverdubPlayCallback(void *instance);
    static void stopCallback(void *instance);
    static void deleteCallback(void *instance);

    void handleRecordOverdubPlay();
    void handleStop();
    void handleDelete();
    void queueJob(Job next);
    void runJob();
    void sendButton(looper_button_action action);

    OneButton recordButton;
    OneButton stopButton;
    MackieShowbox* showbox;
    Print *Debug = &Serial;
    Mode mode = Mode::Empty;
    Job job = Job::None;
    uint8_t jobStep = 0;
    uint32_t jobAt = 0;

    static constexpr uint32_t kTapGapMs = 20;
};

#endif // TWO_BUTTON_LOOPER_H
