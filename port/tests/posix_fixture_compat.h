/* Test fixtures only: the POSIX environment calls the fixtures use, for Windows compilers
 * (force-included by online_menu_test.compiler_command on Windows). */
#ifndef POSIX_FIXTURE_COMPAT_H
#define POSIX_FIXTURE_COMPAT_H
#ifdef _WIN32
#include <stdlib.h>
static inline int setenv(const char* name, const char* value, int overwrite)
{
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
static inline int unsetenv(const char* name) { return _putenv_s(name, "") ? -1 : 0; }
#endif
#endif
