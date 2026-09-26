/* Forced in before Lua's own includes on Android.
   Bionic decides whether strchr is the checked version when <string.h>
   is first included. Lua 5.1 walks length-counted buffers with strchr,
   and the checked version aborts that during luaL_newstate. */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
#define _FORTIFY_SOURCE 0
