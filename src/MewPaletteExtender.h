#ifndef MEW_PALETTE_EXTENDER_H
#define MEW_PALETTE_EXTENDER_H

#include <stdint.h>
#include <windows.h>

#define MOD_NAME "MewPaletteExtender"

#define RVA_IMAGE_DECODE_FROM_MEMORY 0x00A729F0U // Game RVA for the in-memory image decoder hook...
#define RVA_GON_INDEX_BY_NAME_CONST 0x0093EA10U // GonObject::operator[](const std::string&) const...
#define RVA_GON_INDEX_BY_NAME 0x0093EB10U // GonObject::operator[](const std::string&)...
#define RVA_GAME_MALLOC 0x00D35E90U // Game RVA for the allocator...
#define RVA_GAME_FREE 0x00D44914U // Game RVA for the paired free routine...

// Whole-instruction byte count stolen from the image decoder prologue for trampoline...
#define IMAGE_DECODE_HOOK_STOLEN_BYTES 20
#define GON_INDEX_HOOK_STOLEN_BYTES 15

#define PALETTE_WIDTH 16

// (Current game palette is 256 rows tall). Accept that height or larger, allocate custom rows immediately after decoded height...
#define MIN_PALETTE_TEXTURE_ROWS 256
#define MAX_CUSTOM_ROWS 2024
#define MAX_LINE_LENGTH 2024
#define MAX_PATH_LENGTH 520
#define MAX_GON_STRING_LENGTH (2024U * 2024U)

#define GON_INT_DATA_OFFSET 0x50
#define GON_FLOAT_DATA_OFFSET 0x58
#define GON_STRING_DATA_OFFSET 0x68
#define GON_TYPE_OFFSET 0xA8
#define GON_TYPE_STRING 1
#define GON_TYPE_NUMBER 2
#define MSVC_STRING_SIZE_OFFSET 0x10
#define MSVC_STRING_CAPACITY_OFFSET 0x18
#define MSVC_STRING_SSO_CAPACITY 15

__declspec(dllexport) int __cdecl
MewPaletteExtender_ResolvePalette(const char* id, int32_t* resolvedRow);

#endif