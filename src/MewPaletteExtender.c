#include "MewPaletteExtender.h"
#include <string.h>
#include "mewjector.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <wincodec.h>

static const bool ENABLE_DEBUG_LOGS = true;

typedef struct PaletteRow
{
    char id[128];
    char sourcePath[MAX_PATH_LENGTH];
    uint8_t rgba[PALETTE_WIDTH][4];
    int32_t preferredRowIndex;
    int32_t rowIndex;
} PaletteRow;

typedef uint8_t* (__fastcall *fn_image_decode_from_memory)(void* streamRange, int32_t* width, int32_t* height, int32_t* channels, int32_t requestedChannels);
typedef void* (__fastcall *fn_gon_index_by_name)(void* gonObject, const void* fieldName);
typedef void* (__fastcall *fn_game_malloc)(size_t size);
typedef void (__fastcall *fn_game_free)(void* ptr);

static MewjectorAPI g_mj;
static fn_image_decode_from_memory g_origImageDecodeFromMemory = NULL;
static fn_gon_index_by_name g_origGonIndexByNameConst = NULL;
static fn_gon_index_by_name g_origGonIndexByName = NULL;
static fn_game_malloc g_gameMalloc = NULL;
static fn_game_free g_gameFree = NULL;
static PaletteRow g_rows[MAX_CUSTOM_ROWS];
static int32_t g_rowCount = 0;
static int32_t g_paletteBaseHeight = 0;
static int32_t g_requiredHeight = 0;
static volatile LONG g_loadedRows = 0;
static volatile LONG g_finalizedRows = 0;
static volatile LONG g_runtimeInstalled = 0;
static volatile LONG g_decodeLogCount = 0;
static HMODULE g_moduleHandle = NULL;

static int IsValidPaletteId(const char* id);

static void Log(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;

    if (!ENABLE_DEBUG_LOGS || !g_mj.Log)
    {
        return;
    }

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    g_mj.Log(MOD_NAME, "%s", buffer);
}

static UINT_PTR GetGameBase(void)
{
    if (!g_mj.GetGameBase)
    {
        return 0U;
    }

    return g_mj.GetGameBase();
}

static char* TrimInPlace(char* text)
{
    char* end;

    if (!text)
    {
        return text;
    }

    while (*text && isspace((unsigned char)*text))
    {
        ++text;
    }

    end = text + strlen(text);

    while (end > text && isspace((unsigned char)end[-1]))
    {
        --end;
    }

    *end = '\0';
    return text;
}

static char* StripUtf8Bom(char* text)
{
    unsigned char* bytes;

    if (!text)
    {
        return text;
    }

    bytes = (unsigned char*)text;

    if (bytes[0] == 0xEFU && bytes[1] == 0xBBU && bytes[2] == 0xBFU)
    {
        return text + 3;
    }

    return text;
}

static int HexNibble(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }

    if (value >= 'a' && value <= 'f')
    {
        return 10 + value - 'a';
    }

    if (value >= 'A' && value <= 'F')
    {
        return 10 + value - 'A';
    }

    return -1;
}

static int ParseHexByte(const char* text)
{
    int high;
    int low;

    high = HexNibble(text[0]);
    low = HexNibble(text[1]);

    if (high < 0 || low < 0)
    {
        return -1;
    }

    return (high << 4) | low;
}

static int ParseColorToken(const char* token, uint8_t outRgba[4])
{
    int r;
    int g;
    int b;
    int a;
    size_t length;

    if (!token || token[0] != '#')
    {
        return 0;
    }

    length = strlen(token);

    if (length != 7U && length != 9U)
    {
        return 0;
    }

    r = ParseHexByte(token + 1);
    g = ParseHexByte(token + 3);
    b = ParseHexByte(token + 5);
    a = 255;

    if (length == 9U)
    {
        a = ParseHexByte(token + 7);
    }

    if (r < 0 || g < 0 || b < 0 || a < 0)
    {
        return 0;
    }

    outRgba[0] = (uint8_t)r;
    outRgba[1] = (uint8_t)g;
    outRgba[2] = (uint8_t)b;
    outRgba[3] = (uint8_t)a;
    return 1;
}

static int IsAbsolutePath(const char* path)
{
    if (!path || path[0] == '\0')
    {
        return 0;
    }

    if ((isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
        (path[0] == '\\' && path[1] == '\\'))
    {
        return 1;
    }

    return 0;
}

static void GetDirectoryFromPath(const char* path, char* directory, size_t directorySize)
{
    const char* slash;
    const char* backslash;
    size_t length;

    if (!directory || directorySize == 0U)
    {
        return;
    }

    directory[0] = '\0';

    if (!path)
    {
        return;
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');

    if (!slash || (backslash && backslash > slash))
    {
        slash = backslash;
    }

    if (!slash)
    {
        return;
    }

    length = (size_t)(slash - path);

    if (length >= directorySize)
    {
        length = directorySize - 1U;
    }

    memcpy(directory, path, length);
    directory[length] = '\0';
}

static void GetFileNameFromPath(const char* path, char* fileName, size_t fileNameSize)
{
    const char* slash;
    const char* backslash;
    const char* start;

    if (!fileName || fileNameSize == 0U)
    {
        return;
    }

    fileName[0] = '\0';

    if (!path)
    {
        return;
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');

    if (!slash || (backslash && backslash > slash))
    {
        slash = backslash;
    }

    start = slash ? slash + 1 : path;
    snprintf(fileName, fileNameSize, "%s", start);
}

static int FileExists(const char* path)
{
    DWORD attributes;

    if (!path || path[0] == '\0')
    {
        return 0;
    }

    attributes = GetFileAttributesA(path);

    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return 0;
    }

    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U;
}

static void ResolvePaletteAssetPath(const char* manifestPath, const char* assetPath, char* resolvedPath, size_t resolvedPathSize)
{
    char manifestDirectory[MAX_PATH_LENGTH];

    if (!resolvedPath || resolvedPathSize == 0U)
    {
        return;
    }

    resolvedPath[0] = '\0';

    if (!assetPath || assetPath[0] == '\0')
    {
        return;
    }

    if (IsAbsolutePath(assetPath))
    {
        snprintf(resolvedPath, resolvedPathSize, "%s", assetPath);
        return;
    }

    memset(manifestDirectory, 0, sizeof(manifestDirectory));
    GetDirectoryFromPath(manifestPath, manifestDirectory, sizeof(manifestDirectory));

    if (manifestDirectory[0] != '\0')
    {
        snprintf(resolvedPath, resolvedPathSize, "%s\\%s", manifestDirectory, assetPath);
    }
    else
    {
        snprintf(resolvedPath, resolvedPathSize, "%s", assetPath);
    }
}

static int ConvertUtf8PathToWidePath(const char* path, wchar_t* widePath, int32_t widePathCount)
{
    int result;

    if (!path || !widePath || widePathCount <= 0)
    {
        return 0;
    }

    widePath[0] = L'\0';
    result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, widePath, widePathCount);

    if (result <= 0)
    {
        result = MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, widePathCount);
    }

    return result > 0;
}

static void ReleaseUnknown(IUnknown* unknown)
{
    if (unknown)
    {
        unknown->lpVtbl->Release(unknown);
    }
}

static int LoadPngPaletteStrip(const char* path, uint8_t outRgba[PALETTE_WIDTH][4])
{
    IWICImagingFactory* factory;
    IWICBitmapDecoder* decoder;
    IWICBitmapFrameDecode* frame;
    IWICFormatConverter* converter;
    wchar_t widePath[MAX_PATH_LENGTH];
    HRESULT hr;
    UINT width;
    UINT height;
    uint8_t pixels[PALETTE_WIDTH * 4];
    int32_t i;
    int comInitialized;

    if (!path || !outRgba)
    {
        return 0;
    }

    if (!ConvertUtf8PathToWidePath(path, widePath, (int32_t)(sizeof(widePath) / sizeof(widePath[0]))))
    {
        Log("Could not convert palette strip path: %s", path);
        return 0;
    }

    factory = NULL;
    decoder = NULL;
    frame = NULL;
    converter = NULL;
    memset(pixels, 0, sizeof(pixels));

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    comInitialized = SUCCEEDED(hr);

    if (hr == RPC_E_CHANGED_MODE)
    {
        comInitialized = 0;
    }
    else if (FAILED(hr))
    {
        Log("COM init failed while loading palette strip %s: 0x%08X", path, (unsigned int)hr);
        return 0;
    }

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&factory);

    if (SUCCEEDED(hr))
    {
        hr = factory->lpVtbl->CreateDecoderFromFilename(factory, widePath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    }

    if (SUCCEEDED(hr))
    {
        hr = decoder->lpVtbl->GetFrame(decoder, 0U, &frame);
    }

    if (SUCCEEDED(hr))
    {
        hr = frame->lpVtbl->GetSize(frame, &width, &height);
    }

    if (SUCCEEDED(hr) && (width != PALETTE_WIDTH || height != 1U))
    {
        Log("Palette strip must be %dx1 pixels: %s is %ux%u", PALETTE_WIDTH, path, width, height);
        hr = E_INVALIDARG;
    }

    if (SUCCEEDED(hr))
    {
        hr = factory->lpVtbl->CreateFormatConverter(factory, &converter);
    }

    if (SUCCEEDED(hr))
    {
        hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource*)frame, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    }

    if (SUCCEEDED(hr))
    {
        hr = converter->lpVtbl->CopyPixels(converter, NULL, PALETTE_WIDTH * 4U, (UINT)sizeof(pixels), pixels);
    }

    if (SUCCEEDED(hr))
    {
        for (i = 0; i < PALETTE_WIDTH; ++i)
        {
            outRgba[i][0] = pixels[(i * 4) + 0];
            outRgba[i][1] = pixels[(i * 4) + 1];
            outRgba[i][2] = pixels[(i * 4) + 2];
            outRgba[i][3] = pixels[(i * 4) + 3];
        }

        Log("Loaded palette strip: %s", path);
    }
    else
    {
        Log("Failed to load palette strip %s: 0x%08X", path, (unsigned int)hr);
    }

    ReleaseUnknown((IUnknown*)converter);
    ReleaseUnknown((IUnknown*)frame);
    ReleaseUnknown((IUnknown*)decoder);
    ReleaseUnknown((IUnknown*)factory);

    if (comInitialized)
    {
        CoUninitialize();
    }

    return SUCCEEDED(hr);
}

static int ComparePaletteRows(const void* leftValue, const void* rightValue)
{
    const PaletteRow* left;
    const PaletteRow* right;
    int comparison;

    left = (const PaletteRow*)leftValue;
    right = (const PaletteRow*)rightValue;
    comparison = _stricmp(left->id, right->id);

    if (comparison != 0)
    {
        return comparison;
    }

    comparison = _stricmp(left->sourcePath, right->sourcePath);

    if (comparison != 0)
    {
        return comparison;
    }

    comparison = strcmp(left->sourcePath, right->sourcePath);

    if (comparison != 0)
    {
        return comparison;
    }

    if (left->preferredRowIndex != right->preferredRowIndex)
    {
        return left->preferredRowIndex < right->preferredRowIndex ? -1 : 1;
    }

    return memcmp(left->rgba, right->rgba, sizeof(left->rgba));
}

static int CollectPaletteRow(const char* id, int32_t preferredRowIndex, const uint8_t rgba[PALETTE_WIDTH][4], const char* sourcePath)
{
    PaletteRow* row;

    if (!id || !rgba || !sourcePath)
    {
        return 0;
    }

    if (g_rowCount >= MAX_CUSTOM_ROWS)
    {
        Log("Too many custom palette rows!!! max=%d", MAX_CUSTOM_ROWS);
        return 0;
    }

    row = &g_rows[g_rowCount];
    memset(row, 0, sizeof(*row));
    strncpy(row->id, id, sizeof(row->id) - 1U);
    strncpy(row->sourcePath, sourcePath, sizeof(row->sourcePath) - 1U);
    memcpy(row->rgba, rgba, sizeof(row->rgba));
    row->preferredRowIndex = preferredRowIndex;
    row->rowIndex = -1;
    ++g_rowCount;
    return 1;
}

static uint32_t HashPaletteId(const char* id)
{
    uint32_t hash;

    hash = 2166136261U;

    while (*id)
    {
        hash ^= (uint32_t)(unsigned char)tolower((unsigned char)*id);
        hash *= 16777619U;
        ++id;
    }

    return hash;
}

static void FinalizePaletteRows(int32_t paletteBaseHeight)
{
    uint8_t occupied[MAX_CUSTOM_ROWS];
    int32_t readIndex;
    int32_t uniqueCount;

    if (g_rowCount <= 0)
    {
        return;
    }

    qsort(g_rows, (size_t)g_rowCount, sizeof(g_rows[0]), ComparePaletteRows);
    uniqueCount = 0;

    for (readIndex = 0; readIndex < g_rowCount; ++readIndex)
    {
        PaletteRow* candidate;

        candidate = &g_rows[readIndex];

        if (uniqueCount > 0 && _stricmp(g_rows[uniqueCount - 1].id, candidate->id) == 0)
        {
            PaletteRow* selected;
            int sameDefinition;

            selected = &g_rows[uniqueCount - 1];
            sameDefinition = selected->preferredRowIndex == candidate->preferredRowIndex && memcmp(selected->rgba, candidate->rgba, sizeof(selected->rgba)) == 0;

            if (sameDefinition)
            {
                Log("Duplicate palette id has the same definition!!! Using %s and ignoring %s: %s", selected->sourcePath, candidate->sourcePath, selected->id);
            }
            else
            {
                Log("Conflicting palette id!!! Deterministic winner is %s and ignored definition is %s: %s", selected->sourcePath, candidate->sourcePath, selected->id);
            }

            continue;
        }

        if (uniqueCount != readIndex)
        {
            g_rows[uniqueCount] = *candidate;
        }

        ++uniqueCount;
    }

    g_rowCount = uniqueCount;
    memset(occupied, 0, sizeof(occupied));
    g_paletteBaseHeight = paletteBaseHeight;
    g_requiredHeight = paletteBaseHeight;

    for (readIndex = 0; readIndex < g_rowCount; ++readIndex)
    {
        PaletteRow* row;
        int32_t preferredOffset;

        row = &g_rows[readIndex];
        row->rowIndex = -1;

        if (row->preferredRowIndex < 0)
        {
            continue;
        }

        if (row->preferredRowIndex < paletteBaseHeight || row->preferredRowIndex >= paletteBaseHeight + MAX_CUSTOM_ROWS)
        {
            Log("Preferred palette row is outside %d-%d and will be assigned automatically: %s requested=%d", paletteBaseHeight, paletteBaseHeight + MAX_CUSTOM_ROWS - 1, row->id, row->preferredRowIndex);
            continue;
        }

        preferredOffset = row->preferredRowIndex - paletteBaseHeight;

        if (occupied[preferredOffset])
        {
            Log("Preferred palette row collision!!! Assigning this id automatically: %s requested=%d", row->id, row->preferredRowIndex);
            continue;
        }

        occupied[preferredOffset] = 1U;
        row->rowIndex = row->preferredRowIndex;
    }

    for (readIndex = 0; readIndex < g_rowCount; ++readIndex)
    {
        PaletteRow* row;

        row = &g_rows[readIndex];

        if (row->rowIndex < 0)
        {
            uint32_t startOffset;
            int32_t probe;

            startOffset = HashPaletteId(row->id) % MAX_CUSTOM_ROWS;

            for (probe = 0; probe < MAX_CUSTOM_ROWS; ++probe)
            {
                int32_t rowOffset;

                rowOffset = (int32_t)((startOffset + (uint32_t)probe) % MAX_CUSTOM_ROWS);

                if (!occupied[rowOffset])
                {
                    row->rowIndex = paletteBaseHeight + rowOffset;
                    occupied[rowOffset] = 1U;
                    break;
                }
            }

            if (row->rowIndex < 0)
            {
                Log("Could not allocate deterministic row for palette id=%s", row->id);
                continue;
            }
        }

        if ((row->rowIndex + 1) > g_requiredHeight)
        {
            g_requiredHeight = row->rowIndex + 1;
        }

        Log("Registered palette row: @%s => %d source=%s", row->id, row->rowIndex, row->sourcePath);
    }
}

static int ParsePaletteLine(char* line, const char* manifestPath)
{
    char* equals;
    char* atSign;
    char* id;
    char* colors;
    char* token;
    char* context;
    int32_t explicitRowIndex;
    int32_t colorIndex;
    uint8_t rgba[PALETTE_WIDTH][4];
    char stripPath[MAX_PATH_LENGTH];

    id = TrimInPlace(StripUtf8Bom(line));

    if (!id || id[0] == '\0' || id[0] == '#')
    {
        return 0;
    }

    equals = strchr(id, '=');

    if (!equals)
    {
        Log("Skipping palette line without equals: %s", id);
        return 0;
    }

    *equals = '\0';
    colors = TrimInPlace(equals + 1);
    explicitRowIndex = -1;

    atSign = strchr(id, '@');

    if (atSign)
    {
        char* rowText;
        char* rowEnd;

        *atSign = '\0';

        rowText = TrimInPlace(atSign + 1);
        rowEnd = NULL;
        explicitRowIndex = (int32_t)strtol(rowText, &rowEnd, 10);
        rowEnd = TrimInPlace(rowEnd);

        if (rowText[0] == '\0' || rowEnd[0] != '\0')
        {
            Log("Bad explicit row index for palette %s: %s", id, rowText);
            return 0;
        }
    }

    id = TrimInPlace(id);

    if (id[0] == '\0')
    {
        return 0;
    }

    memset(rgba, 0, sizeof(rgba));

    if (colors[0] != '#')
    {
        ResolvePaletteAssetPath(manifestPath, colors, stripPath, sizeof(stripPath));

        if (!LoadPngPaletteStrip(stripPath, rgba))
        {
            Log("Palette %s could not load PNG strip: %s", id, stripPath);
            return 0;
        }

        return CollectPaletteRow(id, explicitRowIndex, rgba, manifestPath);
    }

    context = NULL;
    colorIndex = 0;
    token = strtok_s(colors, " \t,", &context);

    while (token && colorIndex < PALETTE_WIDTH)
    {
        if (!ParseColorToken(token, rgba[colorIndex]))
        {
            Log("Bad color token for %s: %s", id, token);
            return 0;
        }

        ++colorIndex;
        token = strtok_s(NULL, " \t,", &context);
    }

    if (colorIndex != PALETTE_WIDTH)
    {
        Log("Palette %s has %d colors, expected %d", id, colorIndex, PALETTE_WIDTH);
        return 0;
    }

    return CollectPaletteRow(id, explicitRowIndex, rgba, manifestPath);
}

static void LoadPaletteRowsFile(const char* path)
{
    FILE* file;
    char line[MAX_LINE_LENGTH];
    unsigned char bom[2];
    long startPosition;
    int32_t lineNumber;

    SetLastError(0U);
    file = fopen(path, "rb");

    if (!file)
    {
        Log("Could not open palette manifest: %s error=%lu", path, GetLastError());
        return;
    }

    Log("Reading %s", path);

    bom[0] = 0U;
    bom[1] = 0U;
    startPosition = ftell(file);

    if (fread(bom, 1U, 2U, file) == 2U)
    {
        if ((bom[0] == 0xFFU && bom[1] == 0xFEU) || (bom[0] == 0xFEU && bom[1] == 0xFFU))
        {
            Log("Palette manifest is UTF-16, please save as UTF-8 or ANSI: %s", path);
            fclose(file);
            return;
        }
    }

    fseek(file, startPosition, SEEK_SET);
    lineNumber = 0;

    while (fgets(line, sizeof(line), file))
    {
        char* comment;

        ++lineNumber;
        comment = strstr(line, "//");

        if (comment)
        {
            *comment = '\0';
        }

        if (ParsePaletteLine(line, path))
        {
            Log("Accepted palette manifest line %d from %s", lineNumber, path);
        }
    }

    fclose(file);
}

static void LoadPaletteRowsFromDllDirectory(void)
{
    char modulePath[MAX_PATH_LENGTH];
    char* slash;
    char manifestPath[MAX_PATH_LENGTH];

    memset(modulePath, 0, sizeof(modulePath));

    if (!g_moduleHandle)
    {
        return;
    }

    if (GetModuleFileNameA(g_moduleHandle, modulePath, (DWORD)sizeof(modulePath)) == 0U)
    {
        Log("Could not resolve DLL path for DLL-local palette manifest scan");
        return;
    }

    slash = strrchr(modulePath, '\\');

    if (!slash)
    {
        return;
    }

    *slash = '\0';
    snprintf(manifestPath, sizeof(manifestPath), "%s\\palette_rows.txt", modulePath);

    if (FileExists(manifestPath))
    {
        Log("Checking DLL-local palette manifest: %s", manifestPath);
        LoadPaletteRowsFile(manifestPath);
    }
}

static void LoadPaletteRowsFromSiblingModDirectories(void)
{
    char modulePath[MAX_PATH_LENGTH];
    char loaderDirectory[MAX_PATH_LENGTH];
    char modsDirectory[MAX_PATH_LENGTH];
    char loaderFolderName[MAX_PATH_LENGTH];
    char searchPath[MAX_PATH_LENGTH];
    WIN32_FIND_DATAA findData;
    HANDLE findHandle;
    char* slash;

    memset(modulePath, 0, sizeof(modulePath));
    memset(loaderDirectory, 0, sizeof(loaderDirectory));
    memset(modsDirectory, 0, sizeof(modsDirectory));
    memset(loaderFolderName, 0, sizeof(loaderFolderName));

    if (!g_moduleHandle)
    {
        return;
    }

    if (GetModuleFileNameA(g_moduleHandle, modulePath, (DWORD)sizeof(modulePath)) == 0U)
    {
        Log("Could not resolve DLL path for sibling mod palette scan");
        return;
    }

    GetDirectoryFromPath(modulePath, loaderDirectory, sizeof(loaderDirectory));
    GetFileNameFromPath(loaderDirectory, loaderFolderName, sizeof(loaderFolderName));
    snprintf(modsDirectory, sizeof(modsDirectory), "%s", loaderDirectory);

    slash = strrchr(modsDirectory, '\\');

    if (!slash)
    {
        Log("Could not resolve parent mods directory from DLL path: %s", modulePath);
        return;
    }

    *slash = '\0';
    snprintf(searchPath, sizeof(searchPath), "%s\\*", modsDirectory);
    Log("Scanning sibling mod folders for palette manifests: %s", modsDirectory);

    findHandle = FindFirstFileA(searchPath, &findData);

    if (findHandle == INVALID_HANDLE_VALUE)
    {
        Log("Could not enumerate sibling mod folders: %s error=%lu", modsDirectory, GetLastError());
        return;
    }

    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
        {
            char childPath[MAX_PATH_LENGTH];
            char manifestPath[MAX_PATH_LENGTH];

            if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0)
            {
                continue;
            }

            if (_stricmp(findData.cFileName, loaderFolderName) == 0)
            {
                continue;
            }

            snprintf(childPath, sizeof(childPath), "%s\\%s", modsDirectory, findData.cFileName);
            snprintf(manifestPath, sizeof(manifestPath), "%s\\palette_rows.txt", childPath);

            if (FileExists(manifestPath))
            {
                Log("Checking sibling mod palette manifest: %s", manifestPath);
                LoadPaletteRowsFile(manifestPath);
            }
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
}

static void EnsurePaletteRowsLoaded(void)
{
    LONG previousState;

    previousState = InterlockedCompareExchange(&g_loadedRows, 1, 0);

    if (previousState == 0)
    {
        LoadPaletteRowsFromDllDirectory();
        LoadPaletteRowsFromSiblingModDirectories();
        MemoryBarrier();
        InterlockedExchange(&g_loadedRows, 2);
        Log("Palette definition scan complete: definitions=%d", g_rowCount);
        return;
    }

    while (InterlockedCompareExchange(&g_loadedRows, 2, 2) == 1)
    {
        Sleep(0);
    }
}

static int EnsurePaletteRowsFinalized(int32_t paletteBaseHeight)
{
    LONG previousState;

    if (paletteBaseHeight < MIN_PALETTE_TEXTURE_ROWS)
    {
        return 0;
    }

    EnsurePaletteRowsLoaded();
    previousState = InterlockedCompareExchange(&g_finalizedRows, 1, 0);

    if (previousState == 0)
    {
        FinalizePaletteRows(paletteBaseHeight);
        MemoryBarrier();
        InterlockedExchange(&g_finalizedRows, 2);
        Log("Palette rows finalized from decoded texture height: baseHeight=%d rows=%d requiredHeight=%d", g_paletteBaseHeight, g_rowCount, g_requiredHeight);
        return 1;
    }

    while (InterlockedCompareExchange(&g_finalizedRows, 2, 2) == 1)
    {
        Sleep(0);
    }

    if (g_paletteBaseHeight != paletteBaseHeight)
    {
        Log("Ignoring another 16-pixel-wide image after palette allocation: detectedHeight=%d paletteBaseHeight=%d", paletteBaseHeight, g_paletteBaseHeight);
        return 0;
    }

    return 1;
}

static int GetMsvcStringView(const void* stringObject, const char** text, size_t* length)
{
    const uint8_t* bytes;
    size_t stringLength;
    size_t capacity;
    const char* stringData;

    if (!stringObject || !text || !length)
    {
        return 0;
    }

    bytes = (const uint8_t*)stringObject;
    memcpy(&stringLength, bytes + MSVC_STRING_SIZE_OFFSET, sizeof(stringLength));
    memcpy(&capacity, bytes + MSVC_STRING_CAPACITY_OFFSET, sizeof(capacity));

    if (stringLength > MAX_GON_STRING_LENGTH || capacity < stringLength)
    {
        return 0;
    }

    if (capacity <= MSVC_STRING_SSO_CAPACITY)
    {
        stringData = (const char*)bytes;
    }
    else
    {
        memcpy(&stringData, bytes, sizeof(stringData));
    }

    if (!stringData)
    {
        return 0;
    }

    *text = stringData;
    *length = stringLength;
    return 1;
}

static int MsvcStringEqualsLiteral(const void* stringObject, const char* literal)
{
    const char* text;
    size_t length;
    size_t literalLength;

    if (!literal || !GetMsvcStringView(stringObject, &text, &length))
    {
        return 0;
    }

    literalLength = strlen(literal);
    return length == literalLength && memcmp(text, literal, length) == 0;
}

static PaletteRow* FindPaletteRowById(const char* id)
{
    int32_t index;

    for (index = 0; index < g_rowCount; ++index)
    {
        if (_stricmp(g_rows[index].id, id) == 0)
        {
            return &g_rows[index];
        }
    }

    return NULL;
}

__declspec(dllexport) int __cdecl
MewPaletteExtender_ResolvePalette(const char* id, int32_t* resolvedRow)
{
    PaletteRow* row;

    if (!id || !resolvedRow)
    {
        return 0;
    }

    if (*id == '@')
    {
        ++id;
    }

    if (!IsValidPaletteId(id))
    {
        return 0;
    }

    EnsurePaletteRowsLoaded();

    if (InterlockedCompareExchange(&g_finalizedRows, 2, 2) != 2)
    {
        return 0;
    }

    row = FindPaletteRowById(id);

    if (!row || row->rowIndex < g_paletteBaseHeight)
    {
        return 0;
    }

    *resolvedRow = row->rowIndex;
    return 1;
}

static int IsValidPaletteId(const char* id)
{
    const unsigned char* cursor;

    if (!id || id[0] == '\0')
    {
        return 0;
    }

    cursor = (const unsigned char*)id;

    while (*cursor)
    {
        if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-' && *cursor != '.')
        {
            return 0;
        }

        ++cursor;
    }

    return 1;
}

static void MaybeResolveNamedPalette(void* paletteField)
{
    uint8_t* fieldBytes;
    int32_t fieldType;
    const char* token;
    size_t tokenLength;
    char id[128];
    PaletteRow* row;
    int32_t rowIndex;
    double rowAsDouble;

    if (!paletteField)
    {
        return;
    }

    fieldBytes = (uint8_t*)paletteField;
    memcpy(&fieldType, fieldBytes + GON_TYPE_OFFSET, sizeof(fieldType));

    if (fieldType != GON_TYPE_STRING || !GetMsvcStringView(fieldBytes + GON_STRING_DATA_OFFSET, &token, &tokenLength) || tokenLength < 2U || tokenLength >= sizeof(id) || token[0] != '@')
    {
        return;
    }

    memcpy(id, token + 1, tokenLength - 1U);
    id[tokenLength - 1U] = '\0';

    if (!IsValidPaletteId(id))
    {
        Log("Invalid named palette reference: %.*s", (int)tokenLength, token);
        return;
    }

    EnsurePaletteRowsLoaded();

    if (InterlockedCompareExchange(&g_finalizedRows, 2, 2) != 2)
    {
        Log("Named palette requested before the palette texture height was detected: @%s", id);
        return;
    }

    row = FindPaletteRowById(id);

    if (!row || row->rowIndex < g_paletteBaseHeight)
    {
        Log("Unknown named palette reference: @%s", id);
        return;
    }

    rowIndex = row->rowIndex;
    rowAsDouble = (double)rowIndex;
    memcpy(fieldBytes + GON_INT_DATA_OFFSET, &rowIndex, sizeof(rowIndex));
    memcpy(fieldBytes + GON_FLOAT_DATA_OFFSET, &rowAsDouble, sizeof(rowAsDouble));
    MemoryBarrier();
    fieldType = GON_TYPE_NUMBER;
    memcpy(fieldBytes + GON_TYPE_OFFSET, &fieldType, sizeof(fieldType));

    Log("Resolved named palette: @%s => %d", id, rowIndex);
}

static void* __fastcall HookGonIndexByNameConst(void* gonObject, const void* fieldName)
{
    void* field;

    field = g_origGonIndexByNameConst ? g_origGonIndexByNameConst(gonObject, fieldName) : NULL;

    if (field && MsvcStringEqualsLiteral(fieldName, "palette"))
    {
        MaybeResolveNamedPalette(field);
    }

    return field;
}

static void* __fastcall HookGonIndexByName(void* gonObject, const void* fieldName)
{
    void* field;

    field = g_origGonIndexByName ? g_origGonIndexByName(gonObject, fieldName) : NULL;

    if (field && MsvcStringEqualsLiteral(fieldName, "palette"))
    {
        MaybeResolveNamedPalette(field);
    }

    return field;
}

static void FillBlankRow(uint8_t* rowStart, int32_t width, int32_t channels)
{
    int32_t x;

    for (x = 0; x < width; ++x)
    {
        uint8_t* pixel;

        pixel = rowStart + ((size_t)x * (size_t)channels);

        if (channels >= 1)
        {
            pixel[0] = 0;
        }

        if (channels >= 2)
        {
            pixel[1] = 0;
        }

        if (channels >= 3)
        {
            pixel[2] = 0;
        }

        if (channels >= 4)
        {
            pixel[3] = 255;
        }
    }
}

static void WriteCustomRows(uint8_t* pixels, int32_t width, int32_t channels)
{
    int32_t i;

    for (i = 0; i < g_rowCount; ++i)
    {
        PaletteRow* row;
        uint8_t* rowStart;
        int32_t x;

        row = &g_rows[i];
        rowStart = pixels + ((size_t)row->rowIndex * (size_t)width * (size_t)channels);

        for (x = 0; x < PALETTE_WIDTH; ++x)
        {
            uint8_t* pixel;

            pixel = rowStart + ((size_t)x * (size_t)channels);

            if (channels >= 1)
            {
                pixel[0] = row->rgba[x][0];
            }

            if (channels >= 2)
            {
                pixel[1] = row->rgba[x][1];
            }

            if (channels >= 3)
            {
                pixel[2] = row->rgba[x][2];
            }

            if (channels >= 4)
            {
                pixel[3] = row->rgba[x][3];
            }
        }
    }
}

static uint8_t* __fastcall HookImageDecodeFromMemory(void* streamRange, int32_t* width, int32_t* height, int32_t* channels, int32_t requestedChannels)
{
    uint8_t* originalPixels;
    uint8_t* extendedPixels;
    int32_t actualWidth;
    int32_t actualHeight;
    int32_t actualChannels;
    int32_t outputChannels;
    int32_t y;
    size_t oldBytes;
    size_t newBytes;
    size_t rowBytes;

    originalPixels = NULL;

    if (g_origImageDecodeFromMemory)
    {
        originalPixels = g_origImageDecodeFromMemory(streamRange, width, height, channels, requestedChannels);
    }

    if (!originalPixels)
    {
        return originalPixels;
    }

    actualWidth = width ? *width : 0;
    actualHeight = height ? *height : 0;
    actualChannels = channels ? *channels : 0;
    outputChannels = requestedChannels > 0 ? requestedChannels : actualChannels;

    if (InterlockedIncrement(&g_decodeLogCount) <= 32)
    {
        Log("Image decode call: width=%d height=%d channels=%d requested=%d rows=%d", actualWidth, actualHeight, actualChannels, requestedChannels, g_rowCount);
    }

    if (actualWidth != PALETTE_WIDTH || actualHeight < MIN_PALETTE_TEXTURE_ROWS || outputChannels <= 0 || outputChannels > 4)
    {
        return originalPixels;
    }

    EnsurePaletteRowsLoaded();

    if (g_rowCount <= 0)
    {
        return originalPixels;
    }

    if (!EnsurePaletteRowsFinalized(actualHeight))
    {
        return originalPixels;
    }

    Log("Palette image detected: width=%d decodedHeight=%d customStart=%d channels=%d requested=%d", actualWidth, actualHeight, g_paletteBaseHeight, actualChannels, requestedChannels);

    if (g_requiredHeight <= actualHeight)
    {
        WriteCustomRows(originalPixels, actualWidth, outputChannels);
        return originalPixels;
    }

    rowBytes = (size_t)actualWidth * (size_t)outputChannels;
    oldBytes = rowBytes * (size_t)actualHeight;
    newBytes = rowBytes * (size_t)g_requiredHeight;
    extendedPixels = (uint8_t*)g_gameMalloc(newBytes);

    if (!extendedPixels)
    {
        Log("Failed to allocate extended palette: bytes=%llu", (unsigned long long)newBytes);
        return originalPixels;
    }

    memcpy(extendedPixels, originalPixels, oldBytes);

    for (y = actualHeight; y < g_requiredHeight; ++y)
    {
        FillBlankRow(extendedPixels + ((size_t)y * rowBytes), actualWidth, outputChannels);
    }

    WriteCustomRows(extendedPixels, actualWidth, outputChannels);

    if (height)
    {
        *height = g_requiredHeight;
    }

    Log("Extended palette.png from decoded height: %dx%d => %dx%d customStart=%d rowsAdded=%d", actualWidth, actualHeight, actualWidth, g_requiredHeight, g_paletteBaseHeight, g_rowCount);
    
    return extendedPixels;
}

static int InstallHooks(void)
{
    void* decodeTrampoline;
    void* gonConstTrampoline;
    void* gonTrampoline;
    UINT_PTR gameBase;

    if (InterlockedCompareExchange(&g_runtimeInstalled, 1, 0) != 0)
    {
        return 1;
    }

    if (!MJ_Resolve(&g_mj))
    {
        InterlockedExchange(&g_runtimeInstalled, 0);
        return 0;
    }

    gameBase = GetGameBase();

    if (!gameBase)
    {
        Log("Game base unavailable, cannot install palette hook!");
        InterlockedExchange(&g_runtimeInstalled, 0);
        return 0;
    }

    g_gameMalloc = (fn_game_malloc)(gameBase + RVA_GAME_MALLOC);
    g_gameFree = (fn_game_free)(gameBase + RVA_GAME_FREE);
    decodeTrampoline = NULL;
    gonConstTrampoline = NULL;
    gonTrampoline = NULL;

    Log("Installing image decode hook at RVA 0x%X stolen=%d", RVA_IMAGE_DECODE_FROM_MEMORY, IMAGE_DECODE_HOOK_STOLEN_BYTES);

    if (!g_mj.InstallHook(RVA_IMAGE_DECODE_FROM_MEMORY, IMAGE_DECODE_HOOK_STOLEN_BYTES, (void*)HookImageDecodeFromMemory, &decodeTrampoline, 10, MOD_NAME))
    {
        Log("Failed to hook image decode RVA 0x%X", RVA_IMAGE_DECODE_FROM_MEMORY);
        InterlockedExchange(&g_runtimeInstalled, 0);
        return 0;
    }

    g_origImageDecodeFromMemory = (fn_image_decode_from_memory)decodeTrampoline;

    if (!g_mj.InstallHook(RVA_GON_INDEX_BY_NAME_CONST, GON_INDEX_HOOK_STOLEN_BYTES, (void*)HookGonIndexByNameConst, &gonConstTrampoline, 10, MOD_NAME))
    {
        Log("Failed to hook const GON field lookup RVA 0x%X", RVA_GON_INDEX_BY_NAME_CONST);
        InterlockedExchange(&g_runtimeInstalled, 0);
        return 0;
    }

    g_origGonIndexByNameConst = (fn_gon_index_by_name)gonConstTrampoline;

    if (!g_mj.InstallHook(RVA_GON_INDEX_BY_NAME, GON_INDEX_HOOK_STOLEN_BYTES, (void*)HookGonIndexByName, &gonTrampoline, 10, MOD_NAME))
    {
        Log("Failed to hook mutable GON field lookup RVA 0x%X", RVA_GON_INDEX_BY_NAME);
        InterlockedExchange(&g_runtimeInstalled, 0);
        return 0;
    }

    g_origGonIndexByName = (fn_gon_index_by_name)gonTrampoline;
    Log("Installed palette image and named GON hooks: decode=%p gonConst=%p gon=%p", decodeTrampoline, gonConstTrampoline, gonTrampoline);
    
    return 1;
}

static void Initialize(void)
{
    if (!MJ_Resolve(&g_mj))
    {
        return;
    }

    InstallHooks();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)hModule;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_moduleHandle = hModule;
        MJ_Resolve(&g_mj);

        if (g_mj.Log)
        {
            g_mj.Log(MOD_NAME, "Loading!");
        }

        Initialize();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (MJ_Resolve(&g_mj) && g_mj.Log)
        {
            g_mj.Log(MOD_NAME, "Unloading!");
        }
    }

    return TRUE;
}
