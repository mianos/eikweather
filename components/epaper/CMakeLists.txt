idf_component_register(
    SRCS "src/Epaper.cpp" "src/Gfx.cpp" "fonts/fonts.cpp"
    INCLUDE_DIRS "include" "fonts"
    REQUIRES esp_driver_spi esp_driver_gpio esp_timer)

# Font tables are pure data. They are `const` so they land in .rodata (flash);
# -Os keeps the generated arrays from being padded/aligned wastefully.
# Verify with: idf.py size-files | grep -i font
set_source_files_properties(fonts/fonts.cpp PROPERTIES COMPILE_OPTIONS "-Os")
