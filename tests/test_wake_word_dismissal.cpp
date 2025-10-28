#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError,
    kDeviceStateInvalid,
};

enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeRealtime,
};

enum AbortReason {
    kAbortReasonWakeWordDetected,
};

struct FakeAudioService {
    std::vector<std::string> events;
    bool wake_word_enabled = false;
    bool wake_word_encoded = false;

    void EncodeWakeWord() { wake_word_encoded = true; }
    std::string GetLastWakeWord() const { return "xiaozhi"; }
    void PlaySound(std::string_view) { events.emplace_back("play_sound"); }
    bool IsAfeWakeWord() const { return false; }
    void ResetDecoder() { events.emplace_back("reset_decoder"); }
    void EnableWakeWordDetection(bool enable) {
        events.emplace_back(std::string("wake_word_detection_") + (enable ? "on" : "off"));
        wake_word_enabled = enable;
    }
};

struct FakeProtocol {
    bool audio_channel_open = false;
    bool open_should_succeed = false;
    std::vector<std::string> events;

    bool IsAudioChannelOpened() const { return audio_channel_open; }
    bool OpenAudioChannel() {
        if (open_should_succeed) {
            audio_channel_open = true;
            events.emplace_back("open_success");
            return true;
        }
        events.emplace_back("open_failure");
        return false;
    }
    void SendWakeWordDetected(const std::string& wake_word) {
        events.emplace_back(std::string("wake_word:") + wake_word);
    }
};

struct TestApplication {
    FakeAudioService audio_service;
    FakeProtocol* protocol = nullptr;
    DeviceState device_state = kDeviceStateUnknown;
    ListeningMode listening_mode = kListeningModeAutoStop;
    bool alarm_playback_active = false;
    int active_alarm_id = -1;

    void SetDeviceState(DeviceState state) { device_state = state; }
    void SetListeningMode(ListeningMode mode) { listening_mode = mode; }
    void AbortSpeaking(AbortReason) {}
    void StopAlarmPlayback() {
        if (!alarm_playback_active) {
            return;
        }
        alarm_playback_active = false;
        active_alarm_id = -1;
        audio_service.ResetDecoder();
    }

    void OnWakeWordDetected() {
        if (!protocol) {
            return;
        }

        if (device_state == kDeviceStateIdle) {
            audio_service.EncodeWakeWord();

            if (!protocol->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol->OpenAudioChannel()) {
                    StopAlarmPlayback();
                    audio_service.EnableWakeWordDetection(true);
                    return;
                }
            }

            auto wake_word = audio_service.GetLastWakeWord();
            protocol->SendWakeWordDetected(wake_word);
            SetListeningMode(kListeningModeAutoStop);
            audio_service.PlaySound("popup");
        } else if (device_state == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonWakeWordDetected);
        } else if (device_state == kDeviceStateActivating) {
            SetDeviceState(kDeviceStateIdle);
        }
    }
};

bool TestStopAlarmPlaybackClearsState() {
    TestApplication app;
    app.alarm_playback_active = true;
    app.active_alarm_id = 42;

    app.StopAlarmPlayback();

    return !app.alarm_playback_active && app.active_alarm_id == -1 &&
           app.audio_service.events.size() == 1 &&
           app.audio_service.events.front() == "reset_decoder";
}

bool TestStopAlarmPlaybackNoopWhenInactive() {
    TestApplication app;

    app.StopAlarmPlayback();

    return app.audio_service.events.empty() && app.active_alarm_id == -1;
}

bool TestWakeWordFailureStopsAlarmAndReenablesDetection() {
    TestApplication app;
    FakeProtocol protocol;
    protocol.open_should_succeed = false;
    app.protocol = &protocol;
    app.device_state = kDeviceStateIdle;
    app.alarm_playback_active = true;
    app.active_alarm_id = 7;

    app.OnWakeWordDetected();

    const std::vector<std::string> expected_audio_events = {
        "reset_decoder", "wake_word_detection_on"};

    return app.audio_service.wake_word_encoded &&
           protocol.events.size() == 1 && protocol.events.front() == "open_failure" &&
           app.audio_service.events == expected_audio_events &&
           !app.alarm_playback_active && app.active_alarm_id == -1 &&
           app.audio_service.wake_word_enabled;
}

bool TestWakeWordOnlinePathSkipsAlarmCleanup() {
    TestApplication app;
    FakeProtocol protocol;
    protocol.open_should_succeed = true;
    app.protocol = &protocol;
    app.device_state = kDeviceStateIdle;
    app.alarm_playback_active = true;
    app.active_alarm_id = 9;

    app.OnWakeWordDetected();

    bool result = true;

    if (protocol.events.size() != 2) {
        std::cerr << "expected two protocol events, got " << protocol.events.size() << std::endl;
        result = false;
    }
    if (protocol.events.empty() || protocol.events.front() != "open_success") {
        std::cerr << "unexpected first protocol event" << std::endl;
        result = false;
    }
    if (protocol.events.size() < 2 || protocol.events.back() != "wake_word:xiaozhi") {
        std::cerr << "unexpected second protocol event" << std::endl;
        result = false;
    }
    if (app.audio_service.events.size() != 1) {
        std::cerr << "expected one audio event, got " << app.audio_service.events.size() << std::endl;
        result = false;
    } else if (app.audio_service.events.back() != "play_sound") {
        std::cerr << "unexpected last audio event: " << app.audio_service.events.back() << std::endl;
        result = false;
    }
    if (!app.alarm_playback_active || app.active_alarm_id != 9) {
        std::cerr << "alarm state mutated unexpectedly" << std::endl;
        result = false;
    }

    return result;
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*func)();
    };

    const NamedTest tests[] = {
        {"StopAlarmPlayback clears playback state", TestStopAlarmPlaybackClearsState},
        {"StopAlarmPlayback is no-op when inactive", TestStopAlarmPlaybackNoopWhenInactive},
        {"Wake-word dismissal handles offline failure", TestWakeWordFailureStopsAlarmAndReenablesDetection},
        {"Wake-word dismissal preserves alarm during online flow", TestWakeWordOnlinePathSkipsAlarmCleanup},
    };

    for (const auto& test : tests) {
        if (!test.func()) {
            std::cerr << "Test failed: " << test.name << std::endl;
            return 1;
        }
    }

    std::cout << "All wake-word dismissal tests passed" << std::endl;
    return 0;
}

