// Symbolized crash report on any unhandled fault.
//
// Without this, a fault is just an exit code, and the editor is a GUI app so
// there is nowhere for it to say anything. With it, a crash leaves a .txt naming
// the faulting function and file:line, plus a .dmp for a debugger.
//
// Technique referenced from swine-portable's editor/crashdump.cpp (a sibling
// project); no code was ported. Windows-only — the rest of the editor is
// portable, so this compiles to nothing elsewhere.
#include "crashdump.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace {

char g_dir[MAX_PATH] = "";
const char* (*g_context)() = nullptr;     // optional "what was loaded" line

std::string stamped(const char* ext) {
    time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    lt = *localtime(&t);
#endif
    char buf[MAX_PATH + 64];
    snprintf(buf, sizeof(buf), "%scpcw_mapeditor_crash_%04d%02d%02d_%02d%02d%02d.%s",
             g_dir, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec, ext);
    return buf;
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        default:                              return "UNKNOWN";
    }
}

void writeStack(FILE* f, CONTEXT* ctx) {
    HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    STACKFRAME64 fr;
    memset(&fr, 0, sizeof(fr));
    DWORD machine;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    fr.AddrPC.Offset = ctx->Rip; fr.AddrFrame.Offset = ctx->Rbp; fr.AddrStack.Offset = ctx->Rsp;
#elif defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    fr.AddrPC.Offset = ctx->Eip; fr.AddrFrame.Offset = ctx->Ebp; fr.AddrStack.Offset = ctx->Esp;
#else
    fprintf(f, "  (no stack walker for this architecture)\n");
    SymCleanup(proc); return;
#endif
    fr.AddrPC.Mode = fr.AddrFrame.Mode = fr.AddrStack.Mode = AddrModeFlat;

    char symbuf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
    memset(sym, 0, sizeof(symbuf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 500;

    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machine, proc, thread, &fr, ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (!fr.AddrPC.Offset) break;
        DWORD64 disp = 0;
        const char* name = "??";
        if (SymFromAddr(proc, fr.AddrPC.Offset, &disp, sym)) name = sym->Name;

        IMAGEHLP_LINE64 line; DWORD ldisp = 0;
        memset(&line, 0, sizeof(line)); line.SizeOfStruct = sizeof(line);
        if (SymGetLineFromAddr64(proc, fr.AddrPC.Offset, &ldisp, &line))
            fprintf(f, "  %2d  %-44s  %s:%lu\n", i, name, line.FileName, line.LineNumber);
        else
            fprintf(f, "  %2d  %-44s  +0x%llx\n", i, name, (unsigned long long)disp);
    }
    SymCleanup(proc);
}

void writeReport(EXCEPTION_POINTERS* ep) {
    std::string txt = stamped("txt");
    FILE* f = fopen(txt.c_str(), "w");
    if (!f) return;
    fprintf(f, "CPCW map editor crash report\n\n");
    if (ep && ep->ExceptionRecord) {
        fprintf(f, "exception : %s (0x%08lx)\n",
                exceptionName(ep->ExceptionRecord->ExceptionCode),
                (unsigned long)ep->ExceptionRecord->ExceptionCode);
        fprintf(f, "address   : 0x%p\n", ep->ExceptionRecord->ExceptionAddress);
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2)
            fprintf(f, "operation : %s at 0x%llx\n",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    } else {
        fprintf(f, "exception : (terminate / invalid parameter, no record)\n");
    }
    // What the editor was doing matters more than the stack half the time.
    if (g_context) fprintf(f, "state     : %s\n", g_context());
    fprintf(f, "\nstack:\n");
    if (ep && ep->ContextRecord) writeStack(f, ep->ContextRecord);
    else fprintf(f, "  (no context record)\n");
    fclose(f);

    if (ep) {
        std::string dmp = stamped("dmp");
        HANDLE h = CreateFileA(dmp.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                              MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(h);
        }
    }
    fprintf(stderr, "\n*** crashed — report written to %s ***\n", txt.c_str());
    fflush(stderr);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* ep) {
    writeReport(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() { writeReport(nullptr); _exit(3); }

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                        unsigned int, uintptr_t) {
    writeReport(nullptr);
    _exit(3);
}

} // namespace

void crashdump_install(const char* (*contextFn)()) {
    g_context = contextFn;
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        std::string p(exe);
        size_t sl = p.find_last_of("/\\");
        if (sl != std::string::npos) {
            std::string d = p.substr(0, sl + 1);
            strncpy(g_dir, d.c_str(), sizeof(g_dir) - 1);
        }
    }
    SetUnhandledExceptionFilter(onUnhandled);
    std::set_terminate(onTerminate);
    _set_invalid_parameter_handler(onInvalidParameter);
}

void crashdump_test_fault() {
    volatile int* p = (int*)0;
    *p = 1;                          // deliberate access violation
}

#else   // !_WIN32

void crashdump_install(const char* (*)()) {}
void crashdump_test_fault() { *(volatile int*)0 = 1; }

#endif
