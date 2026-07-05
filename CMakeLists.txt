zephyr_include_directories(include)

if(CONFIG_ZMK_ANIMATION)
    target_sources(app PRIVATE src/core/color.c)
    target_sources(app PRIVATE src/core/engine.c)
    target_sources(app PRIVATE src/core/render.c)
    target_sources(app PRIVATE src/animations/solid.c)
    target_sources(app PRIVATE src/animations/null.c)
endif()

if(CONFIG_ZMK_ANIMATION_MOCK_LED_STRIP)
    target_sources(app PRIVATE src/mock_led_strip.c)
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
