# Common sources (shared between real hardware and QEMU modes)
set(COMMON_SRCS
    "main.cc"
    "application.cc"
    "state_machine.cc"
    "msg_router.cc"
    "face_renderer.cc"
    "reminder_manager.cc"
    "settings.cc"
    "ota_manager.cc"
)

if(CONFIG_HILILY_QEMU_MODE)
    # QEMU mode: use mock implementations for hardware and network
    set(PLATFORM_SRCS
        "qemu_mock/board_mock.cc"
        "qemu_mock/wifi_mock.cc"
        "qemu_mock/battery_mock.cc"
        "qemu_mock/button_mock.cc"
        "qemu_mock/wake_word_mock.cc"
        "qemu_mock/afe_processor_mock.cc"
        "qemu_mock/audio_service_mock.cc"
        "qemu_mock/display_manager_mock.cc"
        "qemu_mock/hilily_mqtt_client_mock.cc"
        "qemu_mock/udp_client_mock.cc"
        "qemu_mock/qemu_console.cc"
    )
else()
    # Real hardware: use actual peripheral drivers
    set(PLATFORM_SRCS
        "board.cc"
        "wifi_manager.cc"
        "battery.cc"
        "button_manager.cc"
        "wake_word.cc"
        "micro_wake_word.cc"
        "afe_processor.cc"
        "audio_service.cc"
        "audio_test.cc"
        "display_manager.cc"
        "hilily_mqtt_client.cc"
        "udp_client.cc"
    )
endif()

idf_component_register(
    SRCS ${COMMON_SRCS} ${PLATFORM_SRCS}
    INCLUDE_DIRS "." "qemu_mock"
    PRIV_REQUIRES
        driver
        esp_timer
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_client
        esp_http_server
        app_update
        mqtt
        lwip
        esp_lcd
        esp_event
        esp_adc
        json
        lvgl
)

if(NOT CONFIG_HILILY_QEMU_MODE)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_SOURCE_DIR}/models/daisy_stream_state_internal_quant.tflite"
        BINARY)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/daisy_intro.wav"
        BINARY)
endif()
