cmake_minimum_required(VERSION 3.24)
project(MewPaletteExtender LANGUAGES C)

add_library(MewPaletteExtender SHARED
  src/MewPaletteExtender.c
)

target_compile_features(MewPaletteExtender PRIVATE c_std_11)

target_compile_definitions(MewPaletteExtender PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

set_target_properties(MewPaletteExtender PROPERTIES
  OUTPUT_NAME "MewPaletteExtender"
)
