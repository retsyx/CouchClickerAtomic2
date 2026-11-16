if(CONFIG_CCA_SCREENKEY)
  zephyr_library()
  zephyr_library_sources(src/screenkey/screenkey.c src/screenkey/face.c)
  # ZMK headers (event manager).
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
