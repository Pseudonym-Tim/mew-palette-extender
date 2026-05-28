#ifndef MEW_PALETTE_EXTENDER_H
#define MEW_PALETTE_EXTENDER_H

#include <stdint.h>
#include <windows.h>

#define MOD_NAME "MewPaletteExtender"

#define RVA_IMAGE_DECODE_FROM_MEMORY 0x00A729F0U // Game RVA for the in-memory image decoder hook...
#define RVA_GAME_MALLOC 0x00D35E90U // Game RVA for the allocator...
#define RVA_GAME_FREE 0x00D44914U // Game RVA for the paired free routine...

// Whole-instruction byte count stolen from the image decoder prologue for trampoline...
#define IMAGE_DECODE_HOOK_STOLEN_BYTES 20

#define VANILLA_PALETTE_ROWS 256
#define PALETTE_WIDTH 16
#define PALETTE_FIRST_CUSTOM_ROW VANILLA_PALETTE_ROWS
#define MAX_CUSTOM_ROWS 1024
#define MAX_EXTENDED_PALETTE_ROWS (VANILLA_PALETTE_ROWS + MAX_CUSTOM_ROWS)
#define MAX_LINE_LENGTH 1024
#define MAX_PATH_LENGTH 520

#endif