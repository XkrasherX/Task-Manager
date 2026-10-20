#pragma once

#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace ProcApi {

    // Ідентичність об'єкта: PID/TID + час створення (ідентифікатори можуть використовуватись повторно).
    struct Key {
        DWORD id;
        ULONGLONG ct;
        bool operator<(const Key& o) const { return id < o.id || (id == o.id && ct < o.ct); }
    };

    struct ProcessInfo {
        DWORD pid = 0;
        std::wstring name, path, owner;
        ULONGLONG createTime = 0;
        double cpu = 0;
        DWORD priorityClass = 0;
        bool priorityKnown = false;
        std::wstring state = L"Невідомо / Н/Д";
        SIZE_T ws = 0, priv = 0, va = 0;
        bool memKnown = false, vaMeasured = false;
        DWORD threads = 0;
        bool isSystem = false;
        bool accessible = false;
        bool finished = false;
        DWORD exitCode = 0;
        ULONGLONG finishedTick = 0;
    };

    struct ThreadInfo {
        DWORD tid = 0, pid = 0;
        ULONGLONG createTime = 0, procCt = 0;
        double cpu = 0;
        ULONGLONG cpuTime100ns = 0;
        int relPriority = 0;
        bool priorityKnown = false;
        LONG basePri = 0;
        ULONGLONG affinity = 0;
        WORD group = 0;
        bool affKnown = false;
        std::wstring state = L"Невідомо / Н/Д";
    };

    struct OpResult {
        std::wstring op, oldV, newV, note;
        DWORD pid = 0, tid = 0;
        bool ok = false;
        DWORD err = 0;
        std::wstring Format() const;   // рядок для журналу
    };

    class Monitor {
    public:
        Monitor();
        ~Monitor();

        // selPid = 0xFFFFFFFF -> потоки не збирати
        void Refresh(DWORD selPid);
        const std::vector<ProcessInfo>& Procs() const { return procs_; }
        const std::vector<ThreadInfo>& Threads() const { return threads_; }
        DWORD Logical() const { return logical_; }
        std::vector<std::wstring> TakeEvents();

        bool GetProcAffinity(const ProcessInfo& p, ULONGLONG& procMask, ULONGLONG& sysMask, DWORD& err);

        OpResult SetProcPriority(const ProcessInfo& p, DWORD cls);
        OpResult SetThreadPriorityOp(const ThreadInfo& t, int rel);
        OpResult SetProcAffinity(const ProcessInfo& p, ULONGLONG mask);
        OpResult SetThreadAffinity(const ThreadInfo& t, ULONGLONG mask);
        OpResult SuspendThreadOp(const ThreadInfo& t);
        OpResult ResumeThreadOp(const ThreadInfo& t);
        OpResult SuspendProcess(const ProcessInfo& p);
        OpResult ResumeProcess(const ProcessInfo& p);
        OpResult Terminate(const ProcessInfo& p);
        OpResult TerminateThreadOp(const ThreadInfo& t);
        OpResult Launch(const std::wstring& exe, const std::wstring& args, const std::wstring& dir,
            bool suspended, DWORD& newPid);

    private:
        struct CpuPrev { ULONGLONG cpu; LONGLONG qpc; };
        struct Tracked { HANDLE h; DWORD pid; ULONGLONG ct; std::wstring name; };
        struct VaCache { SIZE_T va; ULONGLONG tick; };

        HANDLE OpenProc(DWORD pid, ULONGLONG ct, DWORD access, DWORD& err);
        HANDLE OpenThr(DWORD tid, DWORD pid, ULONGLONG ct, DWORD access, DWORD& err);

        LARGE_INTEGER freq_;
        DWORD logical_, selfPid_;
        std::vector<ProcessInfo> procs_, history_;
        std::vector<ThreadInfo> threads_;
        std::map<Key, CpuPrev> prevCpu_, prevThr_;
        std::map<Key, std::pair<std::wstring, bool>> ownerCache_;
        std::map<Key, VaCache> vaCache_;
        std::map<Key, std::map<Key, bool>> susp_;   // процес -> потоки, призупинені цим диспетчером
        std::map<Key, Tracked> tracked_;            // HANDLE-и для перевірки завершення
        std::set<Key> own_;                         // процеси, запущені цим диспетчером
        std::vector<std::wstring> events_;
    };

    void SortProcs(std::vector<ProcessInfo>& v, int col, bool asc);
    void SortThreads(std::vector<ThreadInfo>& v, int col, bool asc);
    std::wstring PriorityClassName(DWORD cls);
    std::wstring ThreadPriorityName(int rel);
    bool IsElevated();

} // namespace ProcApi
