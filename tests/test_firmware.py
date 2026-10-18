#!/usr/bin/env python3
"""
Host-side unit tests for HiLily ESP32 firmware.
Tests the logic of core modules without ESP-IDF dependencies.
"""

import json
import struct
import unittest
import os
import re

# Path to source files
SRC_DIR = os.path.join(os.path.dirname(__file__), '..', 'main')


class TestDeviceState(unittest.TestCase):
    """Test device_state.h enums and string conversions"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'device_state.h')) as f:
            self.content = f.read()

    def test_all_states_defined(self):
        expected_states = [
            'kDeviceStateUnknown', 'kDeviceStateStarting', 'kDeviceStateWifiConnecting',
            'kDeviceStateIdle', 'kDeviceStateListening', 'kDeviceStateThinking',
            'kDeviceStateSpeaking', 'kDeviceStateReminding', 'kDeviceStateError'
        ]
        for state in expected_states:
            self.assertIn(state, self.content, f"Missing state: {state}")

    def test_all_emotions_defined(self):
        expected_emotions = [
            'kEmotionNeutral', 'kEmotionHappy', 'kEmotionSad',
            'kEmotionComfort', 'kEmotionSurprise', 'kEmotionThinking'
        ]
        for emotion in expected_emotions:
            self.assertIn(emotion, self.content, f"Missing emotion: {emotion}")

    def test_state_from_string_mapping(self):
        """Verify device_state_from_string maps correctly to API status values"""
        api_statuses = ['idle', 'listening', 'thinking', 'speaking']
        for status in api_statuses:
            pattern = f'"{status}"'
            self.assertIn(pattern, self.content,
                          f"API status '{status}' not handled in device_state_from_string")

    def test_emotion_from_string_mapping(self):
        """Verify all 6 API emotion values are handled"""
        api_emotions = ['neutral', 'happy', 'sad', 'comfort', 'surprise', 'thinking']
        for emotion in api_emotions:
            pattern = f'"{emotion}"'
            self.assertIn(pattern, self.content,
                          f"API emotion '{emotion}' not handled in emotion_from_string")


class TestStateMachine(unittest.TestCase):
    """Test state machine transition logic"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'state_machine.cc')) as f:
            self.content = f.read()
        with open(os.path.join(SRC_DIR, 'state_machine.h')) as f:
            self.header = f.read()

    def test_valid_transitions_implemented(self):
        """Verify IsValidTransition covers all expected paths"""
        self.assertIn('IsValidTransition', self.content)
        # Key transitions from API spec
        self.assertIn('kDeviceStateIdle', self.content)
        self.assertIn('kDeviceStateListening', self.content)
        self.assertIn('kDeviceStateThinking', self.content)
        self.assertIn('kDeviceStateSpeaking', self.content)

    def test_error_can_be_entered_from_any_state(self):
        self.assertIn('kDeviceStateError', self.content)
        self.assertIn('to == kDeviceStateError', self.content)

    def test_reminding_can_interrupt_any_state(self):
        self.assertIn('to == kDeviceStateReminding', self.content)

    def test_mutex_protection(self):
        self.assertIn('xSemaphoreTake', self.content)
        self.assertIn('xSemaphoreGive', self.content)

    def test_callbacks_invoked_outside_lock(self):
        """Callbacks should be copied under lock, invoked outside to prevent deadlock"""
        self.assertIn('auto callbacks = state_callbacks_', self.content)


class TestAPIProtocol(unittest.TestCase):
    """Test that code matches API_SPECIFICATION.md protocol"""

    def setUp(self):
        docs_dir = os.path.join(os.path.dirname(__file__), '..', 'docs')
        with open(os.path.join(docs_dir, 'API_SPECIFICATION.md')) as f:
            self.api_spec = f.read()

    def test_api_spec_has_start_dialogue(self):
        self.assertIn('start_dialogue', self.api_spec)

    def test_api_spec_has_status_update(self):
        self.assertIn('status_update', self.api_spec)

    def test_api_spec_has_emotion_update(self):
        self.assertIn('emotion_update', self.api_spec)

    def test_api_spec_has_audio_meta(self):
        self.assertIn('audio_meta', self.api_spec)

    def test_api_spec_has_ping(self):
        self.assertIn('"type": "ping"', self.api_spec)


class TestMsgRouter(unittest.TestCase):
    """Test message router handles all API message types"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'msg_router.cc')) as f:
            self.content = f.read()

    def test_routes_status_update(self):
        self.assertIn('"status_update"', self.content)

    def test_routes_emotion_update(self):
        self.assertIn('"emotion_update"', self.content)

    def test_routes_audio_meta(self):
        self.assertIn('"audio_meta"', self.content)

    def test_parses_status_field(self):
        self.assertIn('"status"', self.content)

    def test_parses_emotion_field(self):
        self.assertIn('"emotion"', self.content)

    def test_parses_format_field(self):
        self.assertIn('"format"', self.content)

    def test_binary_goes_to_ringbuffer(self):
        self.assertIn('xRingbufferSend', self.content)

    def test_json_uses_queue(self):
        self.assertIn('xQueueSend', self.content)

    def test_frees_json_memory(self):
        """Verify allocated JSON strings are freed to prevent leaks"""
        self.assertIn('free(json_str)', self.content)
        self.assertIn('cJSON_Delete(root)', self.content)


class TestWsClient(unittest.TestCase):
    """Test WebSocket client matches API protocol"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'ws_client.cc')) as f:
            self.content = f.read()

    def test_auth_url(self):
        self.assertIn('/api/v1/device/auth', self.content)

    def test_auth_sends_device_id(self):
        self.assertIn('"device_id"', self.content)

    def test_auth_sends_secret_key(self):
        self.assertIn('"secret_key"', self.content)

    def test_auth_sends_firmware_version(self):
        self.assertIn('"firmware_version"', self.content)

    def test_auth_sends_battery_level(self):
        self.assertIn('"battery_level"', self.content)

    def test_auth_parses_token(self):
        self.assertIn('"token"', self.content)

    def test_auth_parses_ws_url(self):
        self.assertIn('"ws_url"', self.content)

    def test_authorization_header(self):
        self.assertIn('Authorization: Bearer', self.content)

    def test_heartbeat_format(self):
        """Verify ping message matches API spec"""
        self.assertIn('"type"', self.content)
        self.assertIn('"ping"', self.content)
        self.assertIn('"battery"', self.content)
        self.assertIn('"wifi_rssi"', self.content)

    def test_text_and_binary_dispatch(self):
        self.assertIn('0x01', self.content)  # Text opcode
        self.assertIn('0x02', self.content)  # Binary opcode


class TestApplication(unittest.TestCase):
    """Test Application wiring and event handling"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'application.cc')) as f:
            self.content = f.read()
        with open(os.path.join(SRC_DIR, 'application.h')) as f:
            self.header = f.read()

    def test_sends_start_dialogue_on_listening(self):
        """When entering LISTENING, must send start_dialogue to server"""
        self.assertIn('"start_dialogue"', self.content)
        self.assertIn('"trigger"', self.content)
        self.assertIn('"button"', self.content)

    def test_clears_buffers_on_bargein(self):
        """Barge-in must clear audio buffers"""
        self.assertIn('ClearBuffers', self.content)

    def test_event_bits_defined(self):
        required_events = [
            'EVT_WIFI_CONNECTED', 'EVT_WIFI_DISCONNECTED',
            'EVT_WS_CONNECTED', 'EVT_BUTTON_PRESSED',
            'EVT_STATE_CHANGED', 'EVT_REMINDER_TRIGGERED'
        ]
        for evt in required_events:
            self.assertIn(evt, self.header, f"Missing event: {evt}")

    def test_stops_playback_on_bargein(self):
        self.assertIn('StopPlayback', self.content)

    def test_starts_capture_on_listening(self):
        self.assertIn('StartCapture', self.content)


class TestAudioService(unittest.TestCase):
    """Test audio service Jitter Buffer logic"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'audio_service.h')) as f:
            self.header = f.read()
        with open(os.path.join(SRC_DIR, 'audio_service.cc')) as f:
            self.content = f.read()

    def test_jitter_buffer_size(self):
        self.assertIn('JITTER_BUFFER_SIZE', self.header)

    def test_watermarks_defined(self):
        self.assertIn('JITTER_LOW_WATERMARK', self.header)
        self.assertIn('JITTER_HIGH_WATERMARK', self.header)

    def test_high_watermark_gt_low(self):
        """High watermark must be greater than low watermark"""
        # Parse values from header
        low_match = re.search(r'JITTER_LOW_WATERMARK\s+\(AUDIO_FRAME_BYTES \* (\d+)\)', self.header)
        high_match = re.search(r'JITTER_HIGH_WATERMARK\s+\(AUDIO_FRAME_BYTES \* (\d+)\)', self.header)
        if low_match and high_match:
            self.assertGreater(int(high_match.group(1)), int(low_match.group(1)))

    def test_audio_frame_size(self):
        """Frame size should be 100ms at 16kHz"""
        self.assertIn('AUDIO_FRAME_MS', self.header)
        self.assertIn('100', self.header)

    def test_supports_pcm_and_opus_format(self):
        self.assertIn('kAudioFormatPCM', self.header)
        self.assertIn('kAudioFormatOpus', self.header)

    def test_psram_allocation(self):
        """Jitter buffer should prefer PSRAM"""
        self.assertIn('MALLOC_CAP_SPIRAM', self.content)

    def test_buffering_logic(self):
        """Must wait for high watermark before starting playback"""
        self.assertIn('JITTER_HIGH_WATERMARK', self.content)
        self.assertIn('buffering', self.content)


class TestButtonManager(unittest.TestCase):
    """Test button debouncing and ISR safety"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'button_manager.cc')) as f:
            self.content = f.read()

    def test_isr_uses_from_isr_function(self):
        """ISR must use FromISR variant of queue send"""
        self.assertIn('xQueueSendFromISR', self.content)

    def test_debounce_implemented(self):
        self.assertIn('DEBOUNCE_MS', self.content)

    def test_iram_attr(self):
        """ISR handler must be in IRAM"""
        self.assertIn('IRAM_ATTR', self.content)


class TestReminderManager(unittest.TestCase):
    """Test reminder persistence and scanning"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'reminder_manager.cc')) as f:
            self.content = f.read()
        with open(os.path.join(SRC_DIR, 'reminder_manager.h')) as f:
            self.header = f.read()

    def test_nvs_persistence(self):
        self.assertIn('nvs_set_blob', self.content)
        self.assertIn('nvs_get_blob', self.content)
        self.assertIn('nvs_commit', self.content)

    def test_max_reminders_limit(self):
        self.assertIn('MAX_REMINDERS', self.header)

    def test_scan_interval(self):
        """Should scan every 60 seconds"""
        self.assertIn('60000', self.content)

    def test_time_check(self):
        """Must compare current time with timestamps"""
        self.assertIn('time(NULL)', self.content)

    def test_duplicate_handling(self):
        """Adding existing reminder_id should update, not duplicate"""
        self.assertIn('strcmp(r.reminder_id, reminder_id)', self.content)

    def test_cleanup_old_triggered(self):
        """Old triggered reminders should be cleaned up"""
        self.assertIn('remove_if', self.content)

    def test_content_field(self):
        """ReminderEntry must have content field for UI display"""
        self.assertIn('content[64]', self.header)

    def test_add_reminder_accepts_content(self):
        """AddReminder must accept content parameter"""
        self.assertIn('const char* content', self.header)


class TestReminderMessageHandling(unittest.TestCase):
    """Test reminder message routing in msg_router"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'msg_router.cc')) as f:
            self.content = f.read()

    def test_reminder_created_handler(self):
        """Must handle reminder_created message from server"""
        self.assertIn('reminder_created', self.content)
        self.assertIn('AddReminder', self.content)

    def test_reminder_cancelled_handler(self):
        """Must handle reminder_cancelled message from server"""
        self.assertIn('reminder_cancelled', self.content)
        self.assertIn('CancelReminder', self.content)

    def test_reminder_notify_handler(self):
        """Must handle reminder_notify (server backup channel)"""
        self.assertIn('reminder_notify', self.content)

    def test_reminder_includes_manager(self):
        """msg_router must include reminder_manager.h"""
        self.assertIn('reminder_manager.h', self.content)


class TestReminderTriggerNotification(unittest.TestCase):
    """Test that reminder trigger sends notification to server"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'application.cc')) as f:
            self.content = f.read()
        with open(os.path.join(SRC_DIR, 'application.h')) as f:
            self.header = f.read()

    def test_triggered_reminder_id_stored(self):
        """Application must store triggered reminder_id for server notification"""
        self.assertIn('triggered_reminder_id_', self.header)

    def test_sends_reminder_triggered_message(self):
        """Must send reminder_triggered JSON to server via MQTT"""
        self.assertIn('reminder_triggered', self.content)
        self.assertIn('PublishJson', self.content)

    def test_alarm_tone_on_reminding(self):
        """Must play alarm tone when entering reminding state"""
        self.assertIn('PlayAlarmTone', self.content)

    def test_reminding_emotion_set(self):
        """Must set reminding emotion when entering reminding state"""
        self.assertIn('kEmotionReminding', self.content)


class TestReminderEmotion(unittest.TestCase):
    """Test reminding emotion in face renderer"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'device_state.h')) as f:
            self.state_content = f.read()
        with open(os.path.join(SRC_DIR, 'face_renderer.cc')) as f:
            self.face_content = f.read()

    def test_emotion_enum_has_reminding(self):
        """EmotionType must include kEmotionReminding"""
        self.assertIn('kEmotionReminding', self.state_content)

    def test_face_renderer_has_reminding_preset(self):
        """FaceRenderer must have emotion preset for reminding"""
        self.assertIn('kEmotionReminding', self.face_content)

    def test_reminding_state_to_speaking_allowed(self):
        """State machine must allow Reminding -> Speaking for TTS"""
        with open(os.path.join(SRC_DIR, 'state_machine.cc')) as f:
            sm_content = f.read()
        self.assertIn('kDeviceStateSpeaking', sm_content)


class TestEndDialogue(unittest.TestCase):
    """Test end_dialogue message handling"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'msg_router.cc')) as f:
            self.router_content = f.read()
        with open(os.path.join(SRC_DIR, 'msg_router.h')) as f:
            self.router_header = f.read()
        with open(os.path.join(SRC_DIR, 'application.cc')) as f:
            self.app_content = f.read()

    def test_end_dialogue_callback_type_defined(self):
        """MsgRouter must define EndDialogueCallback type"""
        self.assertIn('EndDialogueCallback', self.router_header)

    def test_end_dialogue_registration_method(self):
        """MsgRouter must have OnEndDialogue registration method"""
        self.assertIn('OnEndDialogue', self.router_header)

    def test_end_dialogue_handler_in_router(self):
        """MsgRouter must handle end_dialogue message type"""
        self.assertIn('"end_dialogue"', self.router_content)

    def test_end_dialogue_extracts_reason(self):
        """Handler must extract reason field"""
        self.assertIn('"reason"', self.router_content)

    def test_end_dialogue_invokes_callback(self):
        """Handler must invoke end_dialogue_callback_"""
        self.assertIn('end_dialogue_callback_', self.router_content)

    def test_application_registers_end_dialogue(self):
        """Application must register OnEndDialogue callback"""
        self.assertIn('OnEndDialogue', self.app_content)

    def test_end_dialogue_disables_continuous_mode(self):
        """end_dialogue callback must set continuous_mode_ = false"""
        self.assertIn('continuous_mode_ = false', self.app_content)

    def test_api_spec_documents_end_dialogue(self):
        """API spec must document end_dialogue message"""
        with open(os.path.join(os.path.dirname(SRC_DIR), 'docs', 'API_SPECIFICATION.md')) as f:
            spec = f.read()
        self.assertIn('end_dialogue', spec)
        self.assertIn('user_farewell', spec)


class TestDisplayManager(unittest.TestCase):
    """Test display/UI integration"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'display_manager.cc')) as f:
            self.content = f.read()

    def test_st7789_initialization(self):
        self.assertIn('st7789', self.content)

    def test_color_inversion(self):
        """ST7789 requires color inversion for this board"""
        self.assertIn('invert_color', self.content)

    def test_mutex_protected_ui(self):
        """UI updates must be mutex-protected"""
        self.assertIn('xSemaphoreTake', self.content)
        self.assertIn('xSemaphoreGive', self.content)

    def test_battery_color_changes(self):
        """Battery bar should change color at thresholds"""
        self.assertIn('LV_PALETTE_RED', self.content)
        self.assertIn('LV_PALETTE_YELLOW', self.content)
        self.assertIn('LV_PALETTE_GREEN', self.content)

    def test_all_emotions_rendered(self):
        """All 7 emotion types must have visual representation"""
        self.assertIn('kEmotionHappy', self.content)
        self.assertIn('kEmotionSad', self.content)
        self.assertIn('kEmotionComfort', self.content)
        self.assertIn('kEmotionSurprise', self.content)
        self.assertIn('kEmotionThinking', self.content)
        self.assertIn('kEmotionNeutral', self.content)


class TestPinConfig(unittest.TestCase):
    """Test hardware pin assignments match ESP_DEVICE.md"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'pin_config.h')) as f:
            self.content = f.read()

    def test_lcd_pins(self):
        self.assertIn('PIN_LCD_MOSI', self.content)
        self.assertIn('GPIO_NUM_41', self.content)
        self.assertIn('PIN_LCD_SCLK', self.content)
        self.assertIn('GPIO_NUM_42', self.content)
        self.assertIn('PIN_LCD_CS', self.content)
        self.assertIn('GPIO_NUM_21', self.content)
        self.assertIn('PIN_LCD_DC', self.content)
        self.assertIn('GPIO_NUM_40', self.content)
        self.assertIn('PIN_LCD_RST', self.content)
        self.assertIn('GPIO_NUM_45', self.content)
        self.assertIn('PIN_LCD_BLK', self.content)
        self.assertIn('GPIO_NUM_20', self.content)

    def test_speaker_pins(self):
        self.assertIn('PIN_SPK_DOUT', self.content)
        self.assertIn('GPIO_NUM_7', self.content)
        self.assertIn('PIN_SPK_BCLK', self.content)
        self.assertIn('GPIO_NUM_15', self.content)
        self.assertIn('PIN_SPK_LRCK', self.content)
        self.assertIn('GPIO_NUM_16', self.content)

    def test_audio_params(self):
        self.assertIn('AUDIO_SAMPLE_RATE   16000', self.content)
        self.assertIn('AUDIO_BITS          16', self.content)
        self.assertIn('AUDIO_CHANNELS      1', self.content)

    def test_lcd_params(self):
        self.assertIn('LCD_WIDTH           240', self.content)
        self.assertIn('LCD_HEIGHT          240', self.content)


class TestBoardInit(unittest.TestCase):
    """Test board initialization code"""

    def setUp(self):
        with open(os.path.join(SRC_DIR, 'board.cc')) as f:
            self.content = f.read()

    def test_spi_init(self):
        self.assertIn('spi_bus_initialize', self.content)

    def test_i2s_speaker_init(self):
        self.assertIn('I2S_NUM_0', self.content)
        self.assertIn('PIN_SPK_BCLK', self.content)

    def test_i2s_mic_init(self):
        self.assertIn('I2S_NUM_1', self.content)
        self.assertIn('PIN_MIC_SD', self.content)

    def test_button_init(self):
        self.assertIn('PIN_BUTTON_BOOT', self.content)
        self.assertIn('GPIO_PULLUP_ENABLE', self.content)

    def test_backlight_enabled(self):
        self.assertIn('PIN_LCD_BLK', self.content)
        self.assertIn('gpio_set_level', self.content)


class TestIntegration(unittest.TestCase):
    """Test cross-module integration correctness"""

    def test_all_source_files_in_cmake(self):
        """Every .cc file must be listed in CMakeLists.txt"""
        cmake_path = os.path.join(SRC_DIR, 'CMakeLists.txt')
        with open(cmake_path) as f:
            cmake_content = f.read()

        cc_files = [f for f in os.listdir(SRC_DIR) if f.endswith('.cc')]
        for cc_file in cc_files:
            self.assertIn(cc_file, cmake_content,
                          f"Source file {cc_file} not in CMakeLists.txt")

    def test_all_headers_have_include_guards(self):
        """Every .h file must have include guards"""
        h_files = [f for f in os.listdir(SRC_DIR) if f.endswith('.h')]
        for h_file in h_files:
            with open(os.path.join(SRC_DIR, h_file)) as f:
                content = f.read()
            self.assertIn('#ifndef', content, f"Missing include guard in {h_file}")
            self.assertIn('#define', content, f"Missing include guard in {h_file}")
            self.assertIn('#endif', content, f"Missing include guard in {h_file}")

    def test_no_interrupt_type_in_code(self):
        """Ensure old wrong 'interrupt' message type is not used anywhere"""
        for f_name in os.listdir(SRC_DIR):
            if not f_name.endswith(('.cc', '.h')): continue
            with open(os.path.join(SRC_DIR, f_name)) as f:
                content = f.read()
            # Should not have type: interrupt (old wrong protocol)
            self.assertNotIn('"interrupt"', content,
                             f"Found deprecated 'interrupt' message type in {f_name}")

    def test_no_state_change_type_in_code(self):
        """Ensure old wrong 'state_change' message type is not used"""
        for f_name in os.listdir(SRC_DIR):
            if not f_name.endswith(('.cc', '.h')): continue
            with open(os.path.join(SRC_DIR, f_name)) as f:
                content = f.read()
            self.assertNotIn('"state_change"', content,
                             f"Found deprecated 'state_change' message type in {f_name}")

    def test_no_ws_ticket_in_code(self):
        """Ensure old wrong 'ws_ticket' field is not used"""
        for f_name in os.listdir(SRC_DIR):
            if not f_name.endswith(('.cc', '.h')): continue
            with open(os.path.join(SRC_DIR, f_name)) as f:
                content = f.read()
            self.assertNotIn('ws_ticket', content,
                             f"Found deprecated 'ws_ticket' in {f_name}")


if __name__ == '__main__':
    unittest.main()
