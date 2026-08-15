// Symbolized crash report on an unhandled fault. Install as the FIRST thing
// main() does, so a fault during startup is covered too.
//
// `contextFn` is optional: return a short line describing what the editor was
// doing (the loaded map, the last save status). Half of a crash report's value
// is knowing which map was open, and the stack cannot say.
#pragma once

void crashdump_install(const char* (*contextFn)() = nullptr);

// Deliberately fault, so the handler can be checked against the thing it exists
// for. Used by --crashtest.
void crashdump_test_fault();
