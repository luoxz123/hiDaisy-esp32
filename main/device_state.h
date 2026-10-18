#ifndef DEVICE_STATE_H
#define DEVICE_STATE_H

#include <cstring>

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConnecting,
    kDeviceStateIdle,
    kDeviceStateListening,
    kDeviceStateThinking,
    kDeviceStateSpeaking,
    kDeviceStateReminding,
    kDeviceStateError
};

enum EmotionType {
    kEmotionNeutral,
    kEmotionHappy,
    kEmotionSad,
    kEmotionComfort,
    kEmotionSurprise,
    kEmotionThinking,
    kEmotionListening,
    kEmotionReminding
};

inline const char* device_state_to_string(DeviceState state) {
    switch (state) {
        case kDeviceStateUnknown:        return "unknown";
        case kDeviceStateStarting:       return "starting";
        case kDeviceStateWifiConnecting: return "wifi_connecting";
        case kDeviceStateIdle:           return "idle";
        case kDeviceStateListening:      return "listening";
        case kDeviceStateThinking:       return "thinking";
        case kDeviceStateSpeaking:       return "speaking";
        case kDeviceStateReminding:      return "reminding";
        case kDeviceStateError:          return "error";
        default:                         return "unknown";
    }
}

inline DeviceState device_state_from_string(const char* str) {
    if (!str) return kDeviceStateUnknown;
    if (strcmp(str, "idle") == 0)      return kDeviceStateIdle;
    if (strcmp(str, "listening") == 0) return kDeviceStateListening;
    if (strcmp(str, "thinking") == 0)  return kDeviceStateThinking;
    if (strcmp(str, "speaking") == 0)  return kDeviceStateSpeaking;
    return kDeviceStateUnknown;
}

inline EmotionType emotion_from_string(const char* str) {
    if (!str) return kEmotionNeutral;
    if (strcmp(str, "happy") == 0)    return kEmotionHappy;
    if (strcmp(str, "sad") == 0)      return kEmotionSad;
    if (strcmp(str, "comfort") == 0)  return kEmotionComfort;
    if (strcmp(str, "surprise") == 0) return kEmotionSurprise;
    if (strcmp(str, "thinking") == 0) return kEmotionThinking;
    if (strcmp(str, "listening") == 0) return kEmotionListening;
    if (strcmp(str, "reminding") == 0) return kEmotionReminding;
    return kEmotionNeutral;
}

inline const char* emotion_to_string(EmotionType emotion) {
    switch (emotion) {
        case kEmotionHappy:    return "happy";
        case kEmotionSad:      return "sad";
        case kEmotionComfort:  return "comfort";
        case kEmotionSurprise: return "surprise";
        case kEmotionThinking: return "thinking";
        case kEmotionListening: return "listening";
        case kEmotionReminding: return "reminding";
        default:               return "neutral";
    }
}

#endif // DEVICE_STATE_H
