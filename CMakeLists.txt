zephyr_include_directories(include)

if(CONFIG_ZMK_ANIMATION)
    target_sources(app PRIVATE src/core/color.c)
    target_sources(app PRIVATE src/core/engine.c)
    target_sources(app PRIVATE src/core/render.c)
    target_sources(app PRIVATE src/animations/solid.c)
    target_sources(app PRIVATE src/animations/null.c)
    target_sources(app PRIVATE src/animations/compose.c)
    target_sources(app PRIVATE src/animations/battery_status.c)
    target_sources(app PRIVATE src/animations/layer_status.c)
    target_sources(app PRIVATE src/control/control.c)
    target_sources(app PRIVATE src/control/overlay_queue.c)
    target_sources(app PRIVATE src/control/power_policy.c)
    target_sources(app PRIVATE src/behaviors/animation_control.c)
    target_sources(app PRIVATE src/behaviors/animation_trigger.c)
    target_sources(app PRIVATE src/behaviors/animation_layer_status.c)

    if(CONFIG_ZMK_BLE)
        # endpoint.c includes <zephyr/bluetooth/*.h> unconditionally (as v1
        # did), so it is only compiled when BLE is present at all.
        target_sources(app PRIVATE src/animations/endpoint.c)
    endif()

    if(CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS)
        # Kconfig currently nests ZMK_ANIMATION_CUSTOM_SETTINGS inside
        # `if ZMK_ANIMATION` (see Kconfig's note): control.h's public API
        # this file calls only has a linkable definition (real or "no
        # device" stub) inside control.c, which is only built in this
        # `if(CONFIG_ZMK_ANIMATION)` block.
        target_sources(app PRIVATE src/settings/animation_settings.c)
    endif()
endif()

if(CONFIG_ZMK_ANIMATION_MOCK_LED_STRIP)
    target_sources(app PRIVATE src/mock_led_strip.c)
endif()

if(CONFIG_ZMK_ANIMATION_LAYER_STATUS_SPLIT_TEST)
    target_sources(app PRIVATE src/split_test/layer_status_split_test.c)
endif()

if(CONFIG_ZMK_ANIMATION_SETTINGS_TEST)
    target_sources(app PRIVATE src/test/animation_settings_test.c)
endif()

if(CONFIG_ZMK_ANIMATION_STUDIO_RPC)
    file(GLOB_RECURSE C_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/*.c)
    target_sources(app PRIVATE ${C_FILES})

    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
    set(NANOPB_GENERATE_CPP_STANDALONE OFF)

    # NOTE: adding to app target directly causes build issues, so we create a library instead
    zephyr_library()
    file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

    nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
    target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

    # app should depend on the generated proto files
    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
    add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
endif()
