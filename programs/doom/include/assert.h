#ifndef DOOM_LIBC_ASSERT_H
#define DOOM_LIBC_ASSERT_H
void doom_assert_failed(const char* expr, const char* file, int line);
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : doom_assert_failed(#e, __FILE__, __LINE__))
#endif
#endif
