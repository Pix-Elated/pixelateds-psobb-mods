// Lightweight, thread-safe, auto-flushed logger for pixelated_mods.
//
// Writes to pixelated_mods.log next to the add-on DLL. Every line is flushed to
// disk on write, so a crash cannot lose the last log entry — whatever was
// written immediately before the crash is what the user sees in the file.
//
// Also provides a process-wide unhandled exception filter so SEH crashes
// that bubble up through our code get their exception code, address, and
// (where available) module origin logged before the process terminates.

#pragma once

#include <cstdarg>

namespace pso_log {

// Open (or reopen/append to) pixelated_mods.log in the given directory. Safe to
// call multiple times — subsequent calls are no-ops. `addon_dir` may be
// empty; the log falls back to the current working directory in that case.
void Init(const char *addon_dir);

// Flush + close the log file. Called from DLL_PROCESS_DETACH.
void Shutdown();

// Install SetUnhandledExceptionFilter to log uncaught SEH exceptions.
// Idempotent; chains to any previous filter.
void InstallCrashHandler();

// Write one formatted line to the log with a millisecond-precision
// timestamp prefix. Thread-safe. Before Init() is called this is a no-op.
// After a crash the file is flushed, so the last Write() before the crash
// IS visible in pixelated_mods.log.
void Write(const char *fmt, ...);

// Explicit flush (used by the crash handler and at DLL unload).
void Flush();

} // namespace pso_log

// Convenience macro — keeps call sites short and allows compile-time
// disabling later if we ever want a build-without-logging flavor.
#define PSO_LOG(fmt, ...) ::pso_log::Write(fmt, ##__VA_ARGS__)
