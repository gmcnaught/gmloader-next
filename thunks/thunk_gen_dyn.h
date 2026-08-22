#pragma once
#include "so_util.h"
#include "thunk_gen.h"
#include <stdio.h>
#include <stdlib.h>
template<typename D, typename R, typename... Args>
struct ThunkFloatImplPtr;

template<typename D, typename R, typename... Args>
struct ThunkFloatImplPtr<D, R(*)(Args...)>
{
    __attribute__((noinline)) ABI_ATTR static R bridge(Args... args)
    {
        return D::template bridge_impl<Args...>(args...);
    }

    static constexpr bool has_float_args = has_float_arg<R, Args...>::value;
};

template<typename D, typename R, typename... Args>
struct ThunkFloatImplPtr<D, R(*)(Args...) noexcept>
{
    __attribute__((noinline)) ABI_ATTR static R bridge(Args... args)
    {
        return D::template bridge_impl<Args...>(args...);
    }

    static constexpr bool has_float_args = has_float_arg<R, Args...>::value;
};

template<auto Def, typename PFN>
struct ThunkFloatPtr : ThunkFloatImplPtr<ThunkFloatPtr<Def, PFN>, PFN>
{
public:
    static inline PFN func;
#if defined(TRACE_GL)
    static inline char* symname = NULL;
#endif
    template<typename... Args>
    static auto bridge_impl(Args... args)
    {
#if defined(TRACE_GL)
        if (symname) {
            std::cout << symname << "(";
            ((std::cout << args << ", "), ...);
            std::cout << ")\n";
        }
#endif
        return func(args...);
    }
};

// Templates are specialized referencing the static function pointer we're trying
// to resolve, since for these functions we _don't_ want to share thunks.
template <auto F, class T = ThunkFloatPtr<F, std::remove_pointer_t<decltype(F)>>>
uintptr_t select_either_ptr(void *fn, const char *symname)
{
    T::func = (decltype(T::func))fn;
    
#if defined(TRACE_GL)
    // Always thunk when tracing GL - so we can trace gl calls
    if (strcmp(symname, "gl") == 0 || strcmp(symname, "egl") == 0) {
        T::symname = strdup(symname);
        return (uintptr_t)T::bridge;
    }
#endif

    // When we have ABI_ATTR, we will thunk these so that they are called in the
    // correct ABI.
#ifndef NO_ABI_ATTR
    if constexpr (T::has_float_args)
        return (uintptr_t)T::bridge;
#endif

    // No float/double arguments - just use the original function.
    return (uintptr_t)T::func;
}

/* Per-symbol reporting for the resolver below, off by default.
 *
 * WHY IT IS GATED. This used to fprintf one line per unresolved symbol,
 * unconditionally, in every build. Mesa does not provide ~679 of the extension
 * entry points glad asks for, so every launch wrote 679 lines — and on the
 * MiSTer those go to stderr, which launch.sh redirects to a log on the exFAT
 * SD card. stderr is unbuffered, so that is 679 write syscalls at ~1.3 ms
 * each. Measured on .81 2026-08-22: 1400 log writes cost 1.837 s to
 * /media/fat vs 0.019 s to tmpfs, and moving the whole log to tmpfs took the
 * core's time-to-first-drawn-frame from 14.5 s to 10.2 s. Roughly half of that
 * log volume was this one fprintf.
 *
 * The information is still worth having — it is how you find which GL entry
 * points Mesa cannot supply — so it is a runtime switch rather than a deletion:
 * set GMLOADER_DBG_THUNKS=1 to get the per-symbol lines back. Unset, you still
 * get the one-line count from thunk_resolve_report(), which costs one write. */
inline bool thunk_dbg_enabled()
{
    static const bool on = [] {
        const char *v = getenv("GMLOADER_DBG_THUNKS");
        return v && *v && *v != '0';
    }();
    return on;
}

inline int &thunk_unresolved_count()
{
    static int n = 0;
    return n;
}

/* One line instead of hundreds. Call after a resolver pass. */
inline void thunk_resolve_report(const char *what)
{
    fprintf(stderr, "GL thunks (%s): %d symbol(s) unresolved%s\n", what,
            thunk_unresolved_count(),
            thunk_dbg_enabled() ? "" : " (set GMLOADER_DBG_THUNKS=1 to list them)");
    thunk_unresolved_count() = 0;
}

template <auto F>
void *resolve_thunked(const char *symbol, int &index, DynLibFunction tab[], void *(*resolve)(const char *symbol))
{
    void *f = (void*)resolve(symbol);
    if (f) {
        tab[index++] = (DynLibFunction){symbol, select_either_ptr<F>(f, symbol)};
        tab[index] = {NULL};
    } else {
        thunk_unresolved_count()++;
        // Only log first 64 chars of symbol to avoid log flood for long extension names
        if (thunk_dbg_enabled())
            fprintf(stderr, "DBG resolve NULL: %.64s\n", symbol);
    }

    return f;
}
