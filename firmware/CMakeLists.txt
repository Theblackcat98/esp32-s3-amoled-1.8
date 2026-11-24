cmake_minimum_required(VERSION 3.16)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voice_agent_amoled)

# Waveshare BSP v2.0.3 calls BSP_ERROR_CHECK_RETURN_ERR inside bsp_io_expander_init(),
# which GCC 14 (IDF 5.5) flags as -Wint-conversion error. Suppress only on that component.
idf_component_get_property(_ws_bsp_lib waveshare__esp32_s3_touch_amoled_1_8 COMPONENT_LIB)
if(_ws_bsp_lib)
    target_compile_options(${_ws_bsp_lib} PRIVATE -Wno-error=int-conversion)
endif()
