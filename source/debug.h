#pragma once

// Initialize debug log (clears previous log file)
void debug_init(void);

// Write a line to the screen console and log file
void debug_log(const char *fmt, ...);

// Flush and close the log file
void debug_cleanup(void);
