#include "MewPaletteExtender.h"
#include "mewjector.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

static const bool ENABLE_DEBUG_LOGS = true;

typedef struct PaletteRow
{
    char id[128];
    uint8_t rgba[PALETTE_WIDTH][4];
    int32_t rowIndex;
} PaletteRow;

typedef uint8_t* (__fastcall *fn_image_decode_from_memory)(void* streamRange, int32_t* width, int32_t* height, int32_t* channels, int32_t requestedChannels);
typedef void* (__fastcall *fn_game_malloc)(size_t size);
typedef void (__fastcall *fn_game_free)(void* ptr);

static MewjectorAPI g_mj;
static fn_image_decode_from_memory g_origImageDecodeFromMemory = NULL;
static fn_game_malloc g_gameMalloc = NULL;
static fn_game_free g_gameFree = NULL;
static PaletteRow g_rows[MAX_CUSTOM_ROWS];
static int32_t g_rowCount = 0;
static int32_t g_requiredHeight = VANILLA_PALETTE_ROWS;
static volatile LONG g_loadedRows = 0;
static volatile LONG g_runtimeInstalled = 0;
static volatile LONG g_decodeLogCount = 0;
static HMODULE g_moduleHandle = NULL;

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

static int IsRowIndexTaken(int32_t rowIndex)
{
    int32_t i;

    for (i = 0; i < g_rowCount; ++i)
    {
        if (g_rows[i].rowIndex == rowIndex)
        {
            return 1;
        }
    }

    return 0;
}

static int AddPaletteRow(const char* id, int32_t explicitRowIndex, const uint8_t rgba[PALETTE_WIDTH][4])
{
    PaletteRow* row;
    int32_t i;

    if (!id || !rgba)
    {
        return 0;
    }

    for (i = 0; i < g_rowCount; ++i)
    {
        if (_stricmp(g_rows[i].id, id) == 0)
        {
            Log("Duplicate palette id ignored: %s", id);
            return 0;
        }
    }

    if (g_rowCount >= MAX_CUSTOM_ROWS)
    {
        Log("Too many custom palette rows!!! max=%d", MAX_CUSTOM_ROWS);
        return 0;
    }

    row = &g_rows[g_rowCount];
    memset(row, 0, sizeof(*row));
    strncpy(row->id, id, sizeof(row->id) - 1U);
    memcpy(row->rgba, rgba, sizeof(row->rgba));

    if (explicitRowIndex >= PALETTE_FIRST_CUSTOM_ROW)
    {
        if (explicitRowIndex >= MAX_EXTENDED_PALETTE_ROWS)
        {
            Log("Explicit palette row out of safe range ignored: id=%s row=%d maxExclusive=%d", id, explicitRowIndex, MAX_EXTENDED_PALETTE_ROWS);
            return 0;
        }

        if (IsRowIndexTaken(explicitRowIndex))
        {
            Log("Explicit palette row collision ignored: id=%s row=%d", id, explicitRowIndex);
            return 0;
        }

        row->rowIndex = explicitRowIndex;
    }
    else
    {
        row->rowIndex = PALETTE_FIRST_CUSTOM_ROW + g_rowCount;

        while (row->rowIndex < MAX_EXTENDED_PALETTE_ROWS && IsRowIndexTaken(row->rowIndex))
        {
            ++row->rowIndex;
        }
    }

    if (row->rowIndex < PALETTE_FIRST_CUSTOM_ROW || row->rowIndex >= MAX_EXTENDED_PALETTE_ROWS)
    {
        Log("Could not allocate compact row for palette id=%s", id);
        return 0;
    }

    if ((row->rowIndex + 1) > g_requiredHeight)
    {
        g_requiredHeight = row->rowIndex + 1;
    }

    Log("Registered palette row: %s => %d", row->id, row->rowIndex);
    ++g_rowCount;
    return 1;
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

        return AddPaletteRow(id, explicitRowIndex, rgba);
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
        Log("Palette %s has %d colors; expected %d", id, colorIndex, PALETTE_WIDTH);
        return 0;
    }

    return AddPaletteRow(id, explicitRowIndex, rgba);
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
    if (InterlockedCompareExchange(&g_loadedRows, 1, 0) != 0)
    {
        return;
    }

    LoadPaletteRowsFromSiblingModDirectories();
    Log("Palette row scan complete: rows=%d requiredHeight=%d", g_rowCount, g_requiredHeight);
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

    EnsurePaletteRowsLoaded();

    actualWidth = width ? *width : 0;
    actualHeight = height ? *height : 0;
    actualChannels = channels ? *channels : 0;
    outputChannels = requestedChannels > 0 ? requestedChannels : actualChannels;

    if (InterlockedIncrement(&g_decodeLogCount) <= 32)
    {
        Log("Image decode call: width=%d height=%d channels=%d requested=%d rows=%d", actualWidth, actualHeight, actualChannels, requestedChannels, g_rowCount);
    }

    if (g_rowCount <= 0)
    {
        return originalPixels;
    }

    if (actualWidth != PALETTE_WIDTH || actualHeight != VANILLA_PALETTE_ROWS || outputChannels <= 0 || outputChannels > 4)
    {
        return originalPixels;
    }

    Log("Palette-shaped image detected: width=%d height=%d channels=%d requested=%d", actualWidth, actualHeight, actualChannels, requestedChannels);

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

    /*
    * We do NOT free originalPixels here. Decoder uses CRT allocation path at RVA 0x00D44930, and the texture uploader owns the decoded buffer lifetime.
    * Freeing it from this hook can cause an allocator mismatch or double-free crash depending on the exact call path. 
    * Palette is loaded once, so this leak is intentionally tiny and boot-safe...
    */
    Log("Extended palette.png: %dx%d => %dx%d rowsAdded=%d", actualWidth, actualHeight, actualWidth, g_requiredHeight, g_rowCount);
    return extendedPixels;
}

static int InstallHooks(void)
{
    void* decodeTrampoline;
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

    Log("Installing single image decode hook at RVA 0x%X stolen=%d", RVA_IMAGE_DECODE_FROM_MEMORY, IMAGE_DECODE_HOOK_STOLEN_BYTES);

    if (!g_mj.InstallHook(RVA_IMAGE_DECODE_FROM_MEMORY, IMAGE_DECODE_HOOK_STOLEN_BYTES, (void*)HookImageDecodeFromMemory, &decodeTrampoline, 10, MOD_NAME))
    {
        Log("Failed to hook image decode RVA 0x%X", RVA_IMAGE_DECODE_FROM_MEMORY);
        InterlockedExchange(&g_runtimeInstalled, 0);
        return 0;
    }

    g_origImageDecodeFromMemory = (fn_image_decode_from_memory)decodeTrampoline;
    Log("Installed single image decode hook! decode=0x%X trampoline=%p", RVA_IMAGE_DECODE_FROM_MEMORY, decodeTrampoline);
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
            g_mj.Log(MOD_NAME, "Loading sibling mod palette scan build!");
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