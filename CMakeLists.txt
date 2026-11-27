if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_CYCLOTAB app PRIVATE src/cyclotab.c)
  target_sources_ifdef(CONFIG_ZMK_SMOOTH_SCROLL app PRIVATE src/smooth_scroll.c)
endif()
