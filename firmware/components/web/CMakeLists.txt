idf_component_register(SRCS "idf/web.cpp" INCLUDE_DIRS ".." EMBED_TXTFILES "ui/index.html" "ui/clip_encoder.js" PRIV_INCLUDE_DIRS "../../main"
                       REQUIRES engine eyes net storage cat_eyes
                       PRIV_REQUIRES esp_http_server esp_timer esp_system esp_app_format app_update esp_wifi settings log freertos)
