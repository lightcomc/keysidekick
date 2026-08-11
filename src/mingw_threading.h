#ifndef KEYSIDEKICK_MINGW_THREADING_H
#define KEYSIDEKICK_MINGW_THREADING_H

// Win32-backed C++11 threading primitives for MinGW win32-thread model.
// MinGW 8.1 win32-seh ships neither <mutex> nor <condition_variable>.
// This shim provides the subset used by KeySidekick foundation modules.
// CONDITION_VARIABLE functions are loaded dynamically because the MinGW
// 8.1 w32api headers don't declare them (they exist in kernel32 on Vista+).
//
// Modern MinGW-w64 toolchains (winpthreads / posix thread model, e.g. the
// msys2 mingw-w64-x86_64-gcc used by CI) ship fully functional
// <thread>/<mutex>/<condition_variable>. Defining these names in namespace
// std here would collide with libstdc++ there (redefinition of
// 'class std::thread' etc.), so the shim is only used on toolchains without
// native gthread support. libstdc++ exposes that via _GLIBCXX_HAS_GTHREADS,
// which win32-thread MinGW builds leave undefined.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <chrono>

#if defined(_GLIBCXX_HAS_GTHREADS)
// Toolchain provides real C++11 threading (winpthreads): use it.
#include <condition_variable>
#include <mutex>
#include <thread>
#else
// No native gthread support: provide the Win32-backed shim below.

namespace std {
struct defer_lock_t {};
namespace detail {

// CONDITION_VARIABLE is just a pointer-sized opaque struct on Windows.
struct CvHandle { void* Ptr; };

typedef VOID (WINAPI *PfnInitCv)(CvHandle*);
typedef VOID (WINAPI *PfnWakeCv)(CvHandle*);
typedef VOID (WINAPI *PfnWakeAllCv)(CvHandle*);
typedef BOOL (WINAPI *PfnSleepCvCs)(CvHandle*, CRITICAL_SECTION*, DWORD);

struct CvFuncs {
    PfnInitCv init;
    PfnWakeCv wake;
    PfnWakeAllCv wakeAll;
    PfnSleepCvCs sleepCs;

    CvFuncs() {
        HMODULE h = GetModuleHandleW(L"kernel32.dll");
        init = h ? (PfnInitCv)GetProcAddress(h, "InitializeConditionVariable") : NULL;
        wake = h ? (PfnWakeCv)GetProcAddress(h, "WakeConditionVariable") : NULL;
        wakeAll = h ? (PfnWakeAllCv)GetProcAddress(h, "WakeAllConditionVariable") : NULL;
        sleepCs = h ? (PfnSleepCvCs)GetProcAddress(h, "SleepConditionVariableCS") : NULL;
    }
};

inline const CvFuncs& GetCvFuncs() {
    static CvFuncs funcs;
    return funcs;
}

} // namespace detail

class mutex {
public:
    mutex() { InitializeCriticalSection(&cs_); }
    ~mutex() { DeleteCriticalSection(&cs_); }
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
    bool try_lock() { return TryEnterCriticalSection(&cs_) != 0; }
    CRITICAL_SECTION* native_handle() { return &cs_; }
private:
    mutex(const mutex&);
    mutex& operator=(const mutex&);
    CRITICAL_SECTION cs_;
};

template <typename M>
class lock_guard {
public:
    explicit lock_guard(M& m) : m_(m) { m_.lock(); }
    ~lock_guard() { m_.unlock(); }
private:
    lock_guard(const lock_guard&);
    lock_guard& operator=(const lock_guard&);
    M& m_;
};

template <typename M>
class unique_lock {
public:
    unique_lock() : m_(NULL), owns_(false) {}
    explicit unique_lock(M& m) : m_(&m), owns_(true) { m_->lock(); }
    unique_lock(M& m, std::defer_lock_t) : m_(&m), owns_(false) {}
    ~unique_lock() { if (owns_ && m_) m_->unlock(); }
    void lock() { if (m_ && !owns_) { m_->lock(); owns_ = true; } }
    void unlock() { if (owns_ && m_) { m_->unlock(); owns_ = false; } }
    bool owns_lock() const { return owns_; }
    M* mutex_ptr() const { return m_; }
    CRITICAL_SECTION* native_handle() { return m_ ? m_->native_handle() : NULL; }
private:
    unique_lock(const unique_lock&);
    unique_lock& operator=(const unique_lock&);
    M* m_;
    bool owns_;
};

class condition_variable {
public:
    condition_variable() {
        const detail::CvFuncs& f = detail::GetCvFuncs();
        if (f.init) f.init(&cv_);
        else cv_.Ptr = NULL;
    }

    void notify_one() {
        const detail::CvFuncs& f = detail::GetCvFuncs();
        if (f.wake) f.wake(&cv_);
    }

    void notify_all() {
        const detail::CvFuncs& f = detail::GetCvFuncs();
        if (f.wakeAll) f.wakeAll(&cv_);
    }

    void wait(unique_lock<mutex>& lock) {
        const detail::CvFuncs& f = detail::GetCvFuncs();
        if (f.sleepCs) f.sleepCs(&cv_, lock.native_handle(), INFINITE);
    }

    template <typename Predicate>
    void wait(unique_lock<mutex>& lock, Predicate pred) {
        while (!pred()) wait(lock);
    }

    template <typename Rep, typename Period>
    bool wait_for(unique_lock<mutex>& lock,
                  const std::chrono::duration<Rep, Period>& rel_time) {
        const detail::CvFuncs& f = detail::GetCvFuncs();
        DWORD ms = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(rel_time).count());
        if (f.sleepCs) return f.sleepCs(&cv_, lock.native_handle(), ms) != 0;
        return false;
    }

    template <typename Rep, typename Period, typename Predicate>
    bool wait_for(unique_lock<mutex>& lock,
                  const std::chrono::duration<Rep, Period>& rel_time,
                  Predicate pred) {
        DWORD ms = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(rel_time).count());
        DWORD deadline = GetTickCount() + ms;
        while (!pred()) {
            DWORD now = GetTickCount();
            if (now >= deadline) return pred();
            DWORD remaining = deadline - now;
            const detail::CvFuncs& f = detail::GetCvFuncs();
            if (f.sleepCs) {
                if (f.sleepCs(&cv_, lock.native_handle(), remaining) == 0) {
                    if (!pred()) return false;
                }
            } else {
                return pred();
            }
        }
        return true;
    }

private:
    condition_variable(const condition_variable&);
    condition_variable& operator=(const condition_variable&);
    detail::CvHandle cv_;
};

} // namespace std

namespace std {
namespace this_thread {
inline void sleep_for(const std::chrono::milliseconds& ms) {
    Sleep(static_cast<DWORD>(ms.count()));
}
}
}

namespace std {

class thread {
public:
    thread() : handle_(NULL), id_(0) {}
    template <typename F>
    explicit thread(F f) : handle_(NULL), id_(0) {
        f_ = new FuncHolder<F>(f);
        handle_ = CreateThread(NULL, 0, &thread::ThreadProc, f_, 0, &id_);
        if (!handle_) { delete f_; f_ = NULL; }
    }
    ~thread() { if (handle_) CloseHandle(handle_); }
    void join() {
        if (handle_) { WaitForSingleObject(handle_, INFINITE); CloseHandle(handle_); handle_ = NULL; }
    }
    bool joinable() const { return handle_ != NULL; }
private:
    struct FuncBase { virtual ~FuncBase() {} virtual void call() = 0; };
    template <typename F>
    struct FuncHolder : FuncBase { F f; FuncHolder(F func) : f(func) {} void call() { f(); } };
    static DWORD WINAPI ThreadProc(LPVOID p) {
        FuncBase* fb = static_cast<FuncBase*>(p);
        fb->call();
        delete fb;
        return 0;
    }
    FuncBase* f_;
    HANDLE handle_;
    DWORD id_;
};

} // namespace std

#endif // defined(_GLIBCXX_HAS_GTHREADS) ? libstdc++ real headers : shim

#endif
