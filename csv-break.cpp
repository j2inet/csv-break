// csv-break.cpp
// Win32/C++ application for breaking up large CSV files into smaller files.
// Uses ANSI escape sequences for colored console output.
// Uses VirtualAlloc for memory management.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ANSI escape sequence color codes
#define ANSI_RESET   "\033[0m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_WHITE   "\033[37m"
#define ANSI_BOLD    "\033[1m"

// Enable ANSI escape sequence processing on the Windows console
static void EnableAnsiColors(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE)
        return;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode))
        return;
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
}

static void PrintUsage(const char* programName)
{
    printf(ANSI_BOLD ANSI_CYAN
           "CSV Break - Large CSV File Splitter\n"
           ANSI_RESET "\n");
    printf("Usage: %s [options] <input-file>\n\n", programName);
    printf("Options:\n");
    printf("  " ANSI_YELLOW "-l <max-lines>" ANSI_RESET
           "    Maximum number of data lines per output file\n");
    printf("  " ANSI_YELLOW "-s <max-size>" ANSI_RESET
           "     Maximum output file size (bytes; append K, M, or G for larger units)\n");
    printf("  " ANSI_YELLOW "-p <prefix>" ANSI_RESET
           "       Output file prefix (default: output)\n");
    printf("  " ANSI_YELLOW "-r" ANSI_RESET
           "                Replicate the header row in every output file\n");
    printf("  " ANSI_YELLOW "-?" ANSI_RESET
           "                Show this help message\n\n");
    printf("Notes:\n");
    printf("  Output files are named <prefix>NNNN.csv, where NNNN is zero-padded to 4 digits.\n");
    printf("  Exactly one of -l or -s must be specified.\n");
}

// Parse a size string: plain integer, or integer followed by K / M / G (case-insensitive).
static UINT64 ParseSize(const char* str)
{
    char* end = NULL;
    UINT64 val = (UINT64)_strtoui64(str, &end, 10);
    if (end && *end != '\0')
    {
        switch (*end | 0x20) // to lower case
        {
        case 'k': val *= (UINT64)1024;                     break;
        case 'm': val *= (UINT64)1024 * 1024;              break;
        case 'g': val *= (UINT64)1024 * 1024 * 1024;       break;
        default:  break;
        }
    }
    return val;
}

// Open a new numbered output file.  Returns INVALID_HANDLE_VALUE on failure.
static HANDLE OpenOutputFile(const char* prefix, int index, char* outPathBuf, SIZE_T bufLen)
{
    _snprintf_s(outPathBuf, bufLen, _TRUNCATE, "%s%04d.csv", prefix, index);
    HANDLE h = CreateFileA(outPathBuf,
                           GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return h;
}

// Write a buffer to a file handle.  Returns FALSE on error.
static BOOL WriteAll(HANDLE hFile, const char* data, SIZE_T len)
{
    const char* p = data;
    SIZE_T remaining = len;
    while (remaining > 0)
    {
        DWORD chunk = (remaining > 0x40000000UL) ? 0x40000000UL : (DWORD)remaining;
        DWORD written = 0;
        if (!WriteFile(hFile, p, chunk, &written, NULL) || written == 0)
            return FALSE;
        p         += written;
        remaining -= written;
    }
    return TRUE;
}

// -----------------------------------------------------------------------
// Core splitting logic.
// pData     : pointer to the entire (or partial) file contents in memory.
// dataLen   : number of bytes in pData (NOT null-terminated here; we handle
//             the very last "line" that may not end with '\n').
// prefix    : output file name prefix.
// maxLines  : max data lines per file (0 = no limit).
// maxSize   : max bytes per file (0 = no limit).
// replicateHeader : whether to copy the first line to every output file.
// pFileIndex: in/out counter for output file numbering (caller initialises to 0).
// phOut     : in/out handle for the currently open output file.
// ppHeader  / pHeaderLen : in/out header line storage.
// pLinesOut : in/out count of data lines written to the current file.
// pSizeOut  : in/out byte count written to the current file (excl. replicated headers).
// Returns FALSE on a fatal write/create error.
// -----------------------------------------------------------------------
static BOOL ProcessBlock(
    const char* pData,    SIZE_T dataLen,
    const char* prefix,
    UINT64      maxLines, UINT64 maxSize,
    BOOL        replicateHeader,
    int*        pFileIndex,
    HANDLE*     phOut,
    char**      ppHeader, SIZE_T* pHeaderLen,
    UINT64*     pLinesOut, UINT64* pSizeOut)
{
    const char* p   = pData;
    const char* end = pData + dataLen;

    while (p < end)
    {
        // Find the end of the current line (inclusive of '\n' if present).
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
            ++lineEnd;
        if (lineEnd < end)
            ++lineEnd; // include the '\n'

        SIZE_T lineLen = (SIZE_T)(lineEnd - p);

        // Handle header line (very first line of the whole file).
        if (*ppHeader == NULL && replicateHeader)
        {
            // Save a copy of the header.
            *ppHeader = (char*)malloc(lineLen + 1);
            if (*ppHeader == NULL)
            {
                printf(ANSI_RED "Error: malloc failed for header storage.\n" ANSI_RESET);
                return FALSE;
            }
            memcpy(*ppHeader, p, lineLen);
            (*ppHeader)[lineLen] = '\0';
            *pHeaderLen = lineLen;
        }

        // Decide whether we need to roll to a new output file.
        BOOL needNewFile = (*phOut == INVALID_HANDLE_VALUE);
        if (!needNewFile && maxLines > 0 && *pLinesOut >= maxLines)
            needNewFile = TRUE;
        if (!needNewFile && maxSize > 0 && *pSizeOut + lineLen > maxSize)
            needNewFile = TRUE;

        if (needNewFile)
        {
            // Close the previously open file (if any).
            if (*phOut != INVALID_HANDLE_VALUE)
            {
                CloseHandle(*phOut);
                *phOut = INVALID_HANDLE_VALUE;
                printf(ANSI_GREEN "  Closed output file %04d.\n" ANSI_RESET, *pFileIndex);
            }

            // Open the next output file.
            (*pFileIndex)++;
            char outName[MAX_PATH];
            *phOut = OpenOutputFile(prefix, *pFileIndex, outName, sizeof(outName));
            if (*phOut == INVALID_HANDLE_VALUE)
            {
                printf(ANSI_RED "Error: Cannot create '%s' (error %lu).\n" ANSI_RESET,
                       outName, GetLastError());
                return FALSE;
            }
            printf(ANSI_CYAN "  Creating: " ANSI_RESET "%s\n", outName);
            *pLinesOut = 0;
            *pSizeOut  = 0;

            // Replicate the header into this new file (but not into the very
            // first file, where the header is written as a normal data line).
            if (replicateHeader && *ppHeader != NULL && *pFileIndex > 1)
            {
                if (!WriteAll(*phOut, *ppHeader, *pHeaderLen))
                {
                    printf(ANSI_RED "Error: Failed to write header to output file.\n" ANSI_RESET);
                    return FALSE;
                }
                // Header bytes do NOT count toward pSizeOut / pLinesOut limits.
            }
        }

        // Write the line.
        if (!WriteAll(*phOut, p, lineLen))
        {
            printf(ANSI_RED "Error: Failed to write to output file.\n" ANSI_RESET);
            return FALSE;
        }
        (*pLinesOut)++;
        *pSizeOut += lineLen;

        p = lineEnd;
    }

    return TRUE;
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------
int main(int argc, char* argv[])
{
    EnableAnsiColors();

    // ---- Parse command-line arguments ----------------------------------
    const char* inputFile      = NULL;
    UINT64      maxLines       = 0;
    UINT64      maxSize        = 0;
    const char* prefix         = "output";
    BOOL        replicateHeader = FALSE;

    for (int i = 1; i < argc; i++)
    {
        if (_stricmp(argv[i], "-l") == 0 && i + 1 < argc)
        {
            maxLines = _strtoui64(argv[++i], NULL, 10);
        }
        else if (_stricmp(argv[i], "-s") == 0 && i + 1 < argc)
        {
            maxSize = ParseSize(argv[++i]);
        }
        else if (_stricmp(argv[i], "-p") == 0 && i + 1 < argc)
        {
            prefix = argv[++i];
        }
        else if (_stricmp(argv[i], "-r") == 0)
        {
            replicateHeader = TRUE;
        }
        else if (strcmp(argv[i], "-?") == 0 ||
                 _stricmp(argv[i], "--help") == 0 ||
                 _stricmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (argv[i][0] != '-')
        {
            inputFile = argv[i];
        }
        else
        {
            printf(ANSI_RED "Unknown option: %s\n" ANSI_RESET, argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (inputFile == NULL)
    {
        printf(ANSI_RED "Error: No input file specified.\n" ANSI_RESET);
        PrintUsage(argv[0]);
        return 1;
    }
    if (maxLines == 0 && maxSize == 0)
    {
        printf(ANSI_RED "Error: Specify either -l <max-lines> or -s <max-size>.\n" ANSI_RESET);
        PrintUsage(argv[0]);
        return 1;
    }

    // ---- Open input file -----------------------------------------------
    HANDLE hFile = CreateFileA(inputFile,
                               GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                               NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        printf(ANSI_RED "Error: Cannot open '%s' (error %lu).\n" ANSI_RESET,
               inputFile, GetLastError());
        return 1;
    }

    LARGE_INTEGER fileSize = {0};
    if (!GetFileSizeEx(hFile, &fileSize))
    {
        printf(ANSI_RED "Error: Cannot determine file size (error %lu).\n" ANSI_RESET,
               GetLastError());
        CloseHandle(hFile);
        return 1;
    }

    printf(ANSI_BOLD ANSI_CYAN "CSV Break\n" ANSI_RESET);
    printf(ANSI_WHITE "  Input : " ANSI_RESET "%s\n", inputFile);
    printf(ANSI_WHITE "  Size  : " ANSI_RESET "%llu bytes\n", (UINT64)fileSize.QuadPart);
    if (maxLines)
        printf(ANSI_WHITE "  Limit : " ANSI_RESET "%llu lines per file\n", maxLines);
    if (maxSize)
        printf(ANSI_WHITE "  Limit : " ANSI_RESET "%llu bytes per file\n", maxSize);
    printf(ANSI_WHITE "  Prefix: " ANSI_RESET "%s\n", prefix);
    printf(ANSI_WHITE "  Header: " ANSI_RESET "%s\n\n",
           replicateHeader ? "replicated to every output file" : "written to first file only");

    // ---- Determine allocation strategy ---------------------------------
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);

    printf(ANSI_WHITE "  Available RAM: " ANSI_RESET "%llu MB\n\n",
           memStatus.ullAvailPhys / (1024 * 1024));

    // We keep at least 64 MB free to avoid starving the OS.
    const UINT64 RESERVE_BYTES = 64ULL * 1024 * 1024;
    UINT64 neededBytes = (UINT64)fileSize.QuadPart + 1; // +1 for safety
    // On 32-bit builds SIZE_T is 32 bits; guard against truncation.
    BOOL   fullFileMode = (memStatus.ullAvailPhys >= neededBytes + RESERVE_BYTES)
                       && (neededBytes <= (UINT64)SIZE_MAX);

    SIZE_T allocBytes;
    if (fullFileMode)
    {
        allocBytes = (SIZE_T)neededBytes;
        printf(ANSI_GREEN "Memory mode: full-file (%llu bytes).\n" ANSI_RESET,
               (UINT64)allocBytes);
    }
    else
    {
        // Use at most half of available memory, capped at 256 MB, minimum 1 MB.
        UINT64 halfAvail = memStatus.ullAvailPhys / 2;
        if (halfAvail > 256ULL * 1024 * 1024) halfAvail = 256ULL * 1024 * 1024;
        if (halfAvail < 1ULL  * 1024 * 1024)  halfAvail = 1ULL  * 1024 * 1024;
        allocBytes = (SIZE_T)halfAvail;
        printf(ANSI_YELLOW "Memory mode: chunked (%llu MB buffer).\n" ANSI_RESET,
               (UINT64)(allocBytes / (1024 * 1024)));
    }

    // ---- Allocate memory via VirtualAlloc ------------------------------
    LPVOID pBuf = VirtualAlloc(NULL, allocBytes,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (pBuf == NULL)
    {
        printf(ANSI_RED "Error: VirtualAlloc(%llu) failed (error %lu).\n" ANSI_RESET,
               (UINT64)allocBytes, GetLastError());
        CloseHandle(hFile);
        return 1;
    }

    // ---- Shared state for ProcessBlock ---------------------------------
    int    fileIndex   = 0;
    HANDLE hOut        = INVALID_HANDLE_VALUE;
    char*  pHeader     = NULL;
    SIZE_T headerLen   = 0;
    UINT64 linesOut    = 0;
    UINT64 sizeOut     = 0;
    BOOL   ok          = TRUE;

    // ---- Read and process ----------------------------------------------
    if (fullFileMode)
    {
        // Read the entire file into the buffer.
        char*  buf       = (char*)pBuf;
        UINT64 totalRead = 0;
        while (totalRead < (UINT64)fileSize.QuadPart)
        {
            DWORD toRead = (DWORD)min((UINT64)65536, (UINT64)fileSize.QuadPart - totalRead);
            DWORD bytesRead = 0;
            if (!ReadFile(hFile, buf + (SIZE_T)totalRead, toRead, &bytesRead, NULL) || bytesRead == 0)
                break;
            totalRead += bytesRead;
        }
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;

        ok = ProcessBlock(buf, (SIZE_T)totalRead,
                          prefix, maxLines, maxSize, replicateHeader,
                          &fileIndex, &hOut,
                          &pHeader, &headerLen,
                          &linesOut, &sizeOut);
    }
    else
    {
        // Chunked mode: fill buffer, process up to the last complete line,
        // carry the partial line forward to the next iteration.
        char*  buf         = (char*)pBuf;
        SIZE_T bufCapacity = allocBytes - 1; // reserve 1 byte as a safety margin
        SIZE_T carry       = 0;              // bytes of an incomplete line at buf[0]

        while (ok)
        {
            // Read new data after any carried-over bytes.
            SIZE_T available = bufCapacity - carry;
            DWORD  toRead    = (available > 0x40000000UL)
                               ? 0x40000000UL
                               : (DWORD)available;
            DWORD bytesRead  = 0;
            BOOL  readOk     = ReadFile(hFile, buf + carry, toRead, &bytesRead, NULL);

            if (!readOk && GetLastError() != ERROR_HANDLE_EOF)
            {
                printf(ANSI_RED "Error: ReadFile failed (error %lu).\n" ANSI_RESET,
                       GetLastError());
                ok = FALSE;
                break;
            }

            SIZE_T filled = carry + bytesRead;
            if (filled == 0)
                break; // truly empty – nothing left to process

            // Determine how many bytes to process in this pass.
            SIZE_T processLen;
            SIZE_T newCarry;

            if (bytesRead == 0)
            {
                // EOF: process everything remaining, including any partial line.
                processLen = filled;
                newCarry   = 0;
            }
            else
            {
                // Find the last '\n' so we don't split a line across iterations.
                SIZE_T i = filled;
                while (i > 0 && buf[i - 1] != '\n')
                    --i;

                if (i == 0)
                {
                    // No newline in the entire buffer – line exceeds buffer size.
                    // Process everything and carry nothing.
                    processLen = filled;
                    newCarry   = 0;
                }
                else
                {
                    processLen = i;           // bytes up to and including last '\n'
                    newCarry   = filled - i;  // bytes of the next partial line
                }
            }

            // Process complete lines.
            ok = ProcessBlock(buf, processLen,
                              prefix, maxLines, maxSize, replicateHeader,
                              &fileIndex, &hOut,
                              &pHeader, &headerLen,
                              &linesOut, &sizeOut);

            // Slide any partial line to the front of the buffer.
            if (newCarry > 0)
                memmove(buf, buf + processLen, newCarry);
            carry = newCarry;

            if (bytesRead == 0)
                break; // we just processed the last bytes (EOF path above)
        }
    }

    // ---- Close the last output file ------------------------------------
    if (hOut != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hOut);
        printf(ANSI_GREEN "  Closed output file %04d.\n" ANSI_RESET, fileIndex);
    }

    // ---- Clean up ------------------------------------------------------
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    VirtualFree(pBuf, 0, MEM_RELEASE);
    if (pHeader)
        free(pHeader);

    if (ok)
    {
        printf(ANSI_BOLD ANSI_GREEN
               "\nDone. Created %d output file(s).\n"
               ANSI_RESET, fileIndex);
        return 0;
    }
    else
    {
        printf(ANSI_BOLD ANSI_RED
               "\nFailed after creating %d output file(s).\n"
               ANSI_RESET, fileIndex);
        return 1;
    }
}
