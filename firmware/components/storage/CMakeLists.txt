idf_component_register(SRCS "idf/storage.cpp" INCLUDE_DIRS ".." PRIV_INCLUDE_DIRS "../../main"
                       REQUIRES engine clip eyes cat_eyes
                       PRIV_REQUIRES joltwallet__littlefs vfs log freertos)
