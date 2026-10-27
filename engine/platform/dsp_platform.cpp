// Portable replacements for the Windows-only helpers the DSP core links against, used on every
// platform: registry (in-memory, per thread), wide file I/O, wide string conversion, hardware stubs.
// Windows keeps the in-memory registry too: the real one would be shared between DfxDsp instances
// and threads, and would leave keys behind in HKCU.
#include "codedefs.h"
#include "slout.h"
#include <string>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

static std::string narrow(const wchar_t* w) {
    std::string out;
    for (; w && *w; ++w) {
        unsigned c = (unsigned)*w;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}
static std::wstring widen(const char* s) {
    std::wstring out;
    const unsigned char* p = (const unsigned char*)s;
    while (p && *p) {
        unsigned c = *p++, n = 0;
        if (c >= 0xF0) { c &= 7; n = 3; } else if (c >= 0xE0) { c &= 15; n = 2; } else if (c >= 0xC0) { c &= 31; n = 1; }
        while (n-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
        out += (wchar_t)c;
    }
    return out;
}

// ---- strings ----
int pstrConvertToWideCharString(char* in, wchar_t* out, int max) {
    std::wstring w = widen(in); if ((int)w.size() >= max) return NOT_OKAY;
    wcscpy(out, w.c_str()); return OKAY;
}
int pstrConvertToWideCharString_WithAlloc(char* in, wchar_t** out, int* len) {
    std::wstring w = widen(in); *out = (wchar_t*)malloc((w.size() + 1) * sizeof(wchar_t));
    wcscpy(*out, w.c_str()); if (len) *len = (int)w.size() + 1; return OKAY;
}
int pstrConvertWideCharStringToAnsiCharString(wchar_t* in, char* out, int max) {
    std::string s = narrow(in); if ((int)s.size() >= max) return NOT_OKAY;
    strcpy(out, s.c_str()); return OKAY;
}
int pstrCovertUTF8StringToWideCharString_WithAlloc(char* in, wchar_t** out, int* len) { return pstrConvertToWideCharString_WithAlloc(in, out, len); }
int pstrCovertWideCharStringToUTF8String_WithAlloc(wchar_t* in, char** out, int* len) {
    std::string s = narrow(in); *out = (char*)malloc(s.size() + 1); strcpy(*out, s.c_str()); if (len) *len = (int)s.size() + 1; return OKAY;
}
int pstrToUpper_Wide(wchar_t* in, wchar_t* out) {
    size_t i = 0; for (; in[i]; ++i) out[i] = (wchar_t)towupper(in[i]); out[i] = 0; return OKAY;
}
int pstrCalcLocationOfStrInStr_Wide(wchar_t* hay, wchar_t* needle, int start, int* loc, int* found) {
    const wchar_t* p = wcsstr(hay + start, needle);
    *found = p != nullptr; *loc = p ? (int)(p - hay) : 0; return OKAY;
}

// ---- files ----
#ifdef _WIN32
// Text mode, as FxSound: presets get CRLF line ends on Windows (readers accept both). Binary mode
// would make fwprintf write raw UTF-16.
FILE* fileOpen_Wide(wchar_t* path, wchar_t* mode, CSlout*) {
    return _wfopen(path, mode);
}
int fileExist_Wide(wchar_t* path, int* exists) {
    struct _stat st; *exists = _wstat(path, &st) == 0; return OKAY;
}
#else
// Legacy code joins paths with backslashes (valsSave writes "<dir>\<name>.fac"); map them to '/'.
static std::string posixPath(const wchar_t* path) {
    std::string out = narrow(path);
    for (char& c : out) if (c == '\\') c = '/';
    return out;
}
FILE* fileOpen_Wide(wchar_t* path, wchar_t* mode, CSlout*) {
    return fopen(posixPath(path).c_str(), narrow(mode).c_str());
}
int fileExist_Wide(wchar_t* path, int* exists) {
    struct stat st; *exists = stat(posixPath(path).c_str(), &st) == 0; return OKAY;
}
#endif

// ---- registry: in-memory key/value, one table per thread ----
// The DSP keeps settings in this "registry" and re-reads it inside processAudio(). Each thread
// gets its own fixed table: no lock, no allocation after the thread's first access, and a
// DfxDsp used on one thread (the live instance on the capture IOProc) cannot see writes from a
// DfxDsp on another (e.g. the preset reader on the message thread).
// Invariant for callers: drive all registry-touching calls of one DfxDsp (setters, loadPreset,
// processAudio) from a single thread, or re-apply all parameters after moving it to a new thread.
namespace {
constexpr int kRegSlots = 128, kRegKeyLen = 256, kRegValLen = 256;
struct RegSlot { int root; wchar_t key[kRegKeyLen]; wchar_t value[kRegValLen]; };
struct RegTable { RegSlot slots[kRegSlots]; int used; };
thread_local RegTable regTable; // zero-initialised, trivially destructible
RegSlot* regFind(int root, const wchar_t* path) {
    for (int i = 0; i < regTable.used; ++i)
        if (regTable.slots[i].root == root && wcscmp(regTable.slots[i].key, path) == 0) return &regTable.slots[i];
    return nullptr;
}
}
int regCreateKey_Wide(int root, wchar_t* path, wchar_t* value) {
    if (!path || wcslen(path) >= (size_t)kRegKeyLen) return NOT_OKAY;
    RegSlot* s = regFind(root, path);
    if (!s) {
        if (regTable.used == kRegSlots) return NOT_OKAY;
        s = &regTable.slots[regTable.used++];
        s->root = root; wcscpy(s->key, path);
    }
    wcsncpy(s->value, value ? value : L"", kRegValLen - 1); s->value[kRegValLen - 1] = 0;
    return OKAY;
}
int regReadKey_Wide(int root, wchar_t* path, int* exists, wchar_t* out, unsigned int max) {
    RegSlot* s = path ? regFind(root, path) : nullptr;
    if (exists) *exists = s != nullptr;
    if (s && out && max) { wcsncpy(out, s->value, max - 1); out[max - 1] = 0; }
    return OKAY;
}

// ---- hardware (serial DSP card) stubs, unused in software mode ----
extern "C" {
int comHrdEepromReadUlong(short unsigned, short unsigned, unsigned int*) { return NOT_OKAY; }
int comHrdEepromWriteUlong(short unsigned, short unsigned, unsigned int) { return NOT_OKAY; }
int comHrdwrGetChar(int, int*) { return NOT_OKAY; }
}
