/* Minimal replacement for the Windows SDK's EventToken.h.
 * Provides the EventRegistrationToken struct used by WinRT-style event add_/remove_ methods,
 * for toolchains (e.g. mingw-w64 headers bundled with zig cc) that don't ship it. */
#ifndef __EventToken_h__
#define __EventToken_h__

typedef struct EventRegistrationToken {
    __int64 value;
} EventRegistrationToken;

#endif
