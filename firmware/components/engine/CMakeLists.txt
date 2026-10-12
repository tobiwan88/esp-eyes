idf_component_register(SRCS "idf/render_task.cpp" "core/parse.cpp" "core/pool.cpp" INCLUDE_DIRS ".." PRIV_INCLUDE_DIRS "../../main"
                       REQUIRES gfx eyes freertos esp_timer
                       PRIV_REQUIRES settings display storage clip log esp_system esp_driver_gpio espressif__cjson)
