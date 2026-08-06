// Definition of the global OOM hook pointer declared in utils.h. Platforms that
// want a graceful out-of-memory exit (PSP: durable trace + clean ExitGame instead
// of abort()'s black-screen reboot) install their handler at boot; everywhere else
// it stays NULL and safeMalloc keeps its plain abort behavior.
#include <stddef.h>

void (*g_bsOomHook)(const char* file, int line, size_t size) = 0;
