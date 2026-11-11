//
//  Diagnostics.h
//  Dumper
//
//  File-backed diagnostics for the injected dylib.
//
//  Everything the ImGui console shows lives in memory inside the target game
//  process and is lost the moment the process dies, which makes "it crashes
//  when I press Start Dump" impossible to debug. This module mirrors every
//  console line to <Documents>/dumper_diag.log and installs signal/terminate
//  handlers that append a fault report (signal, fault address, current stage,
//  backtrace PCs) before letting the process die normally.
//
//  The backtrace is written as raw program counters together with the offset
//  inside the dylib that contains them, so it can be symbolicated offline
//  against the matching Dumper.dSYM:
//
//      atos -o Dumper.dylib -arch arm64 -l <dumper base> <pc>
//

#pragma once

#include <cstdarg>

/* Installs the log file, alternate signal stack and handlers.
 * Idempotent, thread-safe, also installed automatically when the dylib loads. */
void DiagInstall();

/* Append one already formatted line to the log file (adds timestamp + '\n'). */
void DiagWriteLine(const char* Text);

/* printf-style line, appended to the log file only. */
void DiagLog(const char* fmt, ...);
void DiagLogV(const char* fmt, va_list args);

/* Records the step the dumper is currently executing. Printed by the crash
 * handler, so a crash gives us the exact stage even if the stack is garbage. */
void DiagSetStage(const char* fmt, ...);

/* Records an unrecoverable setup failure (GObjects not found, GNames pattern
 * missing, ...). Callers check DiagHasFatal() and abort generation cleanly
 * instead of dereferencing null pointers. */
void DiagMarkFatal(const char* fmt, ...);
bool DiagHasFatal();

/* Absolute path of the log file, or "" if no writable location was found. */
const char* DiagGetLogPath();
