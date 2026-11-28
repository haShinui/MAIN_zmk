if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_CYCLOTAB app PRIVATE src/cyclotab.c)
  target_sources_ifdef(CONFIG_ZMK_SMOOTH_SCROLL app PRIVATE src/smooth_scroll.c)
endif()

# runs on both halves (global behavior + right-half display)
target_sources_ifdef(CONFIG_ZMK_UC_INDICATOR app PRIVATE src/uc_indicator.c)
