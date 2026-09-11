#pragma once

// Log process RSS/HWM/Size (+ optional system MemAvailable) to BD_LOG("MEM", ...).
// Safe to call from any thread; cheap (status file read). why may be null.
void bd_log_process_memory(const char* why);

// Throttled variant for per-frame paths. interval_ms<=0 disables.
// Returns 1 if a sample was emitted.
int bd_log_process_memory_throttled(const char* why, int interval_ms);
