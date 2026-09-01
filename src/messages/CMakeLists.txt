add_custom_command(
    OUTPUT "${EMASTER_GENERATED_MESSAGES}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EMASTER_GENERATED_DIR}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_messages.py"
        --input "${EMASTER_MESSAGE_CONFIG_FILE}"
        --output "${EMASTER_GENERATED_MESSAGES}"
    DEPENDS
        "${EMASTER_MESSAGE_CONFIG_FILE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_messages.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include/emaster/messages.h"
    COMMENT "生成编译期消息资源"
    VERBATIM)

add_library(emaster_messages STATIC "${EMASTER_GENERATED_MESSAGES}")
add_library(emaster::messages ALIAS emaster_messages)

target_compile_features(emaster_messages PUBLIC c_std_11)
target_include_directories(
    emaster_messages
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_messages)
