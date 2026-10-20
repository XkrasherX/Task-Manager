
#include "ProcApi.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Advapi32.lib")

namespace ProcApi {

    struct TE { DWORD tid; LONG basePri; };

    static ULONGLONG FT(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

    static std::wstring ErrText(DWORD e) {
        wchar_t* b = nullptr;
        DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, e, 0, (LPWSTR)&b, 0, nullptr);
        std::wstring s = n ? std::wstring(b, n) : std::wstring();
        if (b) LocalFree(b);
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
        return s;
    }

    std::wstring PriorityClassName(DWORD c) {
        switch (c) {
        case IDLE_PRIORITY_CLASS: return L"Idle";
        case BELOW_NORMAL_PRIORITY_CLASS: return L"Below normal";
        case NORMAL_PRIORITY_CLASS: return L"Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above normal";
        case HIGH_PRIORITY_CLASS: return L"High";
        case REALTIME_PRIORITY_CLASS: return L"Realtime";
        default: return L"—";
        }
    }

    std::wstring ThreadPriorityName(int p) {
        const wchar_t* n = L"?";
        switch (p) {
        case -15: n = L"Idle"; break;
        case -2: n = L"Lowest"; break;
        case -1: n = L"Below normal"; break;
        case 0: n = L"Normal"; break;
        case 1: n = L"Above normal"; break;
        case 2: n = L"Highest"; break;
        case 15: n = L"Time critical"; break;
        }
        wchar_t b[64];
        swprintf_s(b, L"%s (%d)", n, p);
        return b;
    }

    bool IsElevated() {
        HANDLE t;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
        TOKEN_ELEVATION e; DWORD n = 0;
        bool r = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
        CloseHandle(t);
        return r;
    }

    std::wstring OpResult::Format() const {
        std::wstring s = op + L" | PID=" + std::to_wstring(pid);
        if (tid) s += L" TID=" + std::to_wstring(tid);
        s += L" | " + (oldV.empty() ? std::wstring(L"—") : oldV) + L" → " + (newV.empty() ? std::wstring(L"—") : newV);
        s += L" | " + std::wstring(ok ? L"успішно" : L"помилка");
        if (!note.empty()) s += L" (" + note + L")";
        if (err) s += L" | код " + std::to_wstring(err) + L": " + ErrText(err);
        return s;
    }

    static OpResult MakeR(const wchar_t* op, DWORD pid, DWORD tid) {
        OpResult r; r.op = op; r.pid = pid; r.tid = tid; return r;
    }
    static OpResult Fail(OpResult r, DWORD e) {
        r.ok = false; r.err = e;
        if (e == ERROR_NOT_FOUND) r.note = L"об'єкт зник або PID/TID використано повторно";
        return r;
    }

    static std::map<DWORD, std::vector<TE>> SnapThreads() {
        std::map<DWORD, std::vector<TE>> m;
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (s == INVALID_HANDLE_VALUE) return m;
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(s, &te)) {
            do {
                TE x; x.tid = te.th32ThreadID; x.basePri = te.tpBasePri;
                m[te.th32OwnerProcessID].push_back(x);
            } while (Thread32Next(s, &te));
        }
        CloseHandle(s);
        return m;
    }

    static bool GetOwner(HANDLE hProc, std::wstring& name, bool& isSys) {
        HANDLE tok;
        if (!OpenProcessToken(hProc, TOKEN_QUERY, &tok)) return false;
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len ? len : 1);
        if (!len || !GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) { CloseHandle(tok); return false; }
        PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
        isSys = false;
        WELL_KNOWN_SID_TYPE types[3] = { WinLocalSystemSid, WinLocalServiceSid, WinNetworkServiceSid };
        for (int i = 0; i < 3; i++) {
            BYTE sb[SECURITY_MAX_SID_SIZE]; DWORD sz = sizeof(sb);
            if (CreateWellKnownSid(types[i], nullptr, sb, &sz) && EqualSid(sid, sb)) isSys = true;
        }
        wchar_t n[256], d[256]; DWORD nl = 256, dl = 256; SID_NAME_USE u;
        if (LookupAccountSidW(nullptr, sid, n, &nl, d, &dl, &u)) name = std::wstring(d) + L"\\" + n;
        else name = L"(SID без імені)";
        CloseHandle(tok);
        return true;
    }

    // VA = сума RegionSize ділянок MEM_COMMIT та MEM_RESERVE (MEM_FREE не враховується)
    static bool MeasureVA(DWORD pid, SIZE_T& total) {
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) return false;
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char* addr = nullptr;
        total = 0;
        while (VirtualQueryEx(h, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if (mbi.State == MEM_COMMIT || mbi.State == MEM_RESERVE) total += mbi.RegionSize;
            unsigned char* next = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
        CloseHandle(h);
        return true;
    }

    Monitor::Monitor() {
        QueryPerformanceFrequency(&freq_);
        logical_ = GetActiveProcessorCount(0);   // активні логічні процесори групи 0
        if (logical_ == 0) logical_ = 1;
        if (logical_ > 64) logical_ = 64;
        selfPid_ = GetCurrentProcessId();
    }

    Monitor::~Monitor() {
        for (auto it = tracked_.begin(); it != tracked_.end(); ++it) CloseHandle(it->second.h);
    }

    std::vector<std::wstring> Monitor::TakeEvents() {
        std::vector<std::wstring> v; v.swap(events_); return v;
    }

    HANDLE Monitor::OpenProc(DWORD pid, ULONGLONG ct, DWORD access, DWORD& err) {
        HANDLE h = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) { err = GetLastError(); return nullptr; }
        FILETIME c, e, k, u;
        if (!GetProcessTimes(h, &c, &e, &k, &u)) { err = GetLastError(); CloseHandle(h); return nullptr; }
        if (FT(c) != ct) { err = ERROR_NOT_FOUND; CloseHandle(h); return nullptr; }
        return h;
    }

    HANDLE Monitor::OpenThr(DWORD tid, DWORD pid, ULONGLONG ct, DWORD access, DWORD& err) {
        HANDLE h = OpenThread(access | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!h) { err = GetLastError(); return nullptr; }
        FILETIME c, e, k, u;
        if (!GetThreadTimes(h, &c, &e, &k, &u)) { err = GetLastError(); CloseHandle(h); return nullptr; }
        if (FT(c) != ct || GetProcessIdOfThread(h) != pid) { err = ERROR_NOT_FOUND; CloseHandle(h); return nullptr; }
        return h;
    }

    void Monitor::Refresh(DWORD selPid) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        ULONGLONG tick = GetTickCount64();
        std::map<DWORD, std::vector<TE>> tmap = SnapThreads();
        procs_.clear();
        threads_.clear();
        std::map<Key, CpuPrev> newPrev;
        std::set<Key> aliveKeys;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    ProcessInfo p;
                    p.pid = pe.th32ProcessID;
                    p.name = pe.szExeFile;
                    p.threads = pe.cntThreads;
                    if (p.pid == 0 || p.pid == 4) p.isSystem = true;
                    HANDLE h = p.pid == 0 ? nullptr : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
                    if (h) {
                        p.accessible = true;
                        FILETIME c, e, k, u;
                        if (GetProcessTimes(h, &c, &e, &k, &u)) {
                            p.createTime = FT(c);
                            ULONGLONG tot = FT(k) + FT(u);
                            Key key{ p.pid, p.createTime };
                            auto it = prevCpu_.find(key);
                            if (it != prevCpu_.end()) {
                                double dt = (double)(now.QuadPart - it->second.qpc) * 1e7 / (double)freq_.QuadPart; // у 100 нс
                                if (dt > 0) p.cpu = 100.0 * (double)(tot - it->second.cpu) / (dt * logical_);
                                if (p.cpu < 0) p.cpu = 0;
                                if (p.cpu > 100) p.cpu = 100;
                            }
                            CpuPrev cp; cp.cpu = tot; cp.qpc = now.QuadPart;
                            newPrev[key] = cp;
                        }
                        DWORD pc = GetPriorityClass(h);
                        if (pc) { p.priorityClass = pc; p.priorityKnown = true; }
                        wchar_t pathBuf[MAX_PATH * 2]; DWORD sz = MAX_PATH * 2;
                        if (QueryFullProcessImageNameW(h, 0, pathBuf, &sz)) p.path = pathBuf;
                        PROCESS_MEMORY_COUNTERS_EX pmc; ZeroMemory(&pmc, sizeof(pmc)); pmc.cb = sizeof(pmc);
                        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                            p.ws = pmc.WorkingSetSize; p.priv = pmc.PrivateUsage; p.memKnown = true;
                        }
                        Key key{ p.pid, p.createTime };
                        auto oc = ownerCache_.find(key);
                        if (oc != ownerCache_.end()) { p.owner = oc->second.first; if (oc->second.second) p.isSystem = true; }
                        else {
                            std::wstring n; bool sys = false;
                            if (GetOwner(h, n, sys)) { ownerCache_[key] = std::make_pair(n, sys); p.owner = n; if (sys) p.isSystem = true; }
                            else p.owner = L"Н/Д";
                        }
                        CloseHandle(h);
                        // VA — дорого: лише для обраного процесу і не частіше ніж раз на 10 с; для решти — кеш або «не виміряно»
                        Key vk{ p.pid, p.createTime };
                        if (p.pid == selPid) {
                            auto vc = vaCache_.find(vk);
                            if (vc == vaCache_.end() || tick - vc->second.tick > 10000) {
                                SIZE_T va = 0;
                                if (MeasureVA(p.pid, va)) { VaCache x; x.va = va; x.tick = tick; vaCache_[vk] = x; }
                            }
                        }
                        auto vc2 = vaCache_.find(vk);
                        if (vc2 != vaCache_.end()) { p.va = vc2->second.va; p.vaMeasured = true; }
                        p.state = L"Активний";
                        // стан з урахуванням власних призупинень
                        Key key2{ p.pid, p.createTime };
                        auto sit = susp_.find(key2);
                        if (sit != susp_.end()) {
                            aliveKeys.insert(key2);
                            std::set<DWORD> cur;
                            auto tt = tmap.find(p.pid);
                            if (tt != tmap.end()) for (size_t i = 0; i < tt->second.size(); i++) cur.insert(tt->second[i].tid);
                            int alive = 0;
                            for (auto q = sit->second.begin(); q != sit->second.end();) {
                                if (cur.count(q->first.id)) { alive++; ++q; }
                                else q = sit->second.erase(q);
                            }
                            if (alive > 0) p.state = (alive == (int)cur.size()) ? L"Призупинено цим диспетчером" : L"Частково призупинено";
                        }
                    }
                    else {
                        p.state = (p.pid == 0 || p.pid == 4) ? L"Активний" : L"Невідомо / Н/Д";
                        p.owner = L"Н/Д";
                    }
                    if (p.pid == 0) p.owner = L"—";
                    procs_.push_back(p);
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        prevCpu_.swap(newPrev);
        for (auto it = susp_.begin(); it != susp_.end();) {
            if (!aliveKeys.count(it->first) && !tracked_.count(it->first)) it = susp_.erase(it); else ++it;
        }

        // Перевірка завершення без блокування UI: WaitForSingleObject(h, 0)
        for (auto it = tracked_.begin(); it != tracked_.end();) {
            if (WaitForSingleObject(it->second.h, 0) == WAIT_OBJECT_0) {
                DWORD code = 0; GetExitCodeProcess(it->second.h, &code);
                ProcessInfo hp;
                hp.pid = it->second.pid; hp.name = it->second.name; hp.createTime = it->second.ct;
                hp.finished = true; hp.exitCode = code; hp.accessible = true; hp.finishedTick = tick; hp.owner = L"—";
                hp.state = L"Завершено (код " + std::to_wstring(code) + L")";
                history_.push_back(hp);
                events_.push_back(L"Завершення процесу | PID=" + std::to_wstring(hp.pid) + L" " + hp.name +
                    L" | код завершення " + std::to_wstring(code));
                for (size_t i = 0; i < procs_.size();) {
                    if (procs_[i].pid == hp.pid && procs_[i].createTime == hp.createTime) procs_.erase(procs_.begin() + i); else i++;
                }
                susp_.erase(it->first);
                CloseHandle(it->second.h);
                it = tracked_.erase(it);
            }
            else ++it;
        }
        for (size_t i = 0; i < history_.size();) {
            if (tick - history_[i].finishedTick > 30000) history_.erase(history_.begin() + i); else i++;
        }
        for (size_t i = 0; i < history_.size(); i++) procs_.push_back(history_[i]);

        // Потоки обраного процесу
        if (selPid != 0xFFFFFFFF) {
            ULONGLONG pct = 0; bool found = false;
            for (size_t i = 0; i < procs_.size(); i++)
                if (procs_[i].pid == selPid && !procs_[i].finished) { pct = procs_[i].createTime; found = true; }
            if (found) {
                Key pk{ selPid, pct };
                auto tt = tmap.find(selPid);
                std::map<Key, CpuPrev> newT;
                if (tt != tmap.end()) {
                    for (size_t i = 0; i < tt->second.size(); i++) {
                        const TE& te = tt->second[i];
                        ThreadInfo t;
                        t.tid = te.tid; t.pid = selPid; t.procCt = pct; t.basePri = te.basePri;
                        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.tid);
                        if (h) {
                            FILETIME c, e, k, u;
                            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                                t.createTime = FT(c);
                                t.cpuTime100ns = FT(k) + FT(u);
                                Key tk{ t.tid, t.createTime };
                                auto pv = prevThr_.find(tk);
                                if (pv != prevThr_.end()) {
                                    double dt = (double)(now.QuadPart - pv->second.qpc) * 1e7 / (double)freq_.QuadPart;
                                    if (dt > 0) t.cpu = 100.0 * (double)(t.cpuTime100ns - pv->second.cpu) / (dt * logical_);
                                    if (t.cpu < 0) t.cpu = 0;
                                    if (t.cpu > 100) t.cpu = 100;
                                }
                                CpuPrev cp; cp.cpu = t.cpuTime100ns; cp.qpc = now.QuadPart;
                                newT[tk] = cp;
                            }
                            int pr = GetThreadPriority(h);
                            if (pr != THREAD_PRIORITY_ERROR_RETURN) { t.relPriority = pr; t.priorityKnown = true; }
                            GROUP_AFFINITY ga;
                            if (GetThreadGroupAffinity(h, &ga)) { t.affinity = ga.Mask; t.group = ga.Group; t.affKnown = true; }
                            t.state = L"Активний";
                            CloseHandle(h);
                        }
                        auto sit = susp_.find(pk);
                        if (sit != susp_.end() && sit->second.count(Key{ te.tid, t.createTime }))
                            t.state = L"Призупинено цим диспетчером";
                        threads_.push_back(t);
                    }
                }
                prevThr_.swap(newT);
            }
            else prevThr_.clear();
        }
        else prevThr_.clear();
    }

    bool Monitor::GetProcAffinity(const ProcessInfo& p, ULONGLONG& pm, ULONGLONG& sm, DWORD& err) {
        HANDLE h = OpenProc(p.pid, p.createTime, 0, err);
        if (!h) return false;
        DWORD_PTR a = 0, b = 0;
        BOOL ok = GetProcessAffinityMask(h, &a, &b);
        if (!ok) err = GetLastError();
        CloseHandle(h);
        pm = a; sm = b;
        return ok != FALSE;
    }

    OpResult Monitor::SetProcPriority(const ProcessInfo& p, DWORD cls) {
        OpResult r = MakeR(L"Пріоритет процесу", p.pid, 0);
        DWORD err = 0;
        HANDLE h = OpenProc(p.pid, p.createTime, PROCESS_SET_INFORMATION, err);
        if (!h) return Fail(r, err);
        r.oldV = PriorityClassName(GetPriorityClass(h));
        if (!SetPriorityClass(h, cls)) { r.err = GetLastError(); CloseHandle(h); return r; }
        r.newV = PriorityClassName(GetPriorityClass(h));   // повторне читання
        r.ok = true;
        CloseHandle(h);
        return r;
    }

    OpResult Monitor::SetThreadPriorityOp(const ThreadInfo& t, int rel) {
        OpResult r = MakeR(L"Пріоритет потоку", t.pid, t.tid);
        DWORD err = 0;
        HANDLE h = OpenThr(t.tid, t.pid, t.createTime, THREAD_SET_INFORMATION, err);
        if (!h) return Fail(r, err);
        r.oldV = ThreadPriorityName(GetThreadPriority(h));
        if (!SetThreadPriority(h, rel)) { r.err = GetLastError(); CloseHandle(h); return r; }
        r.newV = ThreadPriorityName(GetThreadPriority(h));
        r.ok = true;
        CloseHandle(h);
        return r;
    }

    static std::wstring MaskText(ULONGLONG m) {
        std::wstring s;
        for (int i = 0; i < 64; i++) if (m & (1ULL << i)) { if (!s.empty()) s += L","; s += std::to_wstring(i); }
        return s.empty() ? L"—" : s;
    }

    OpResult Monitor::SetProcAffinity(const ProcessInfo& p, ULONGLONG mask) {
        OpResult r = MakeR(L"Affinity процесу", p.pid, 0);
        DWORD err = 0;
        HANDLE h = OpenProc(p.pid, p.createTime, PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, err);
        if (!h) return Fail(r, err);
        DWORD_PTR pm = 0, sm = 0;
        if (!GetProcessAffinityMask(h, &pm, &sm)) { r.err = GetLastError(); CloseHandle(h); return r; }
        r.oldV = MaskText(pm);
        if (mask == 0 || (mask & ~(ULONGLONG)sm)) {
            r.err = ERROR_INVALID_PARAMETER; r.note = L"маска порожня або виходить за дозволену"; CloseHandle(h); return r;
        }
        if (!SetProcessAffinityMask(h, (DWORD_PTR)mask)) { r.err = GetLastError(); CloseHandle(h); return r; }
        DWORD_PTR pm2 = 0;
        GetProcessAffinityMask(h, &pm2, &sm);
        r.newV = MaskText(pm2);
        r.ok = true;
        CloseHandle(h);
        return r;
    }

    OpResult Monitor::SetThreadAffinity(const ThreadInfo& t, ULONGLONG mask) {
        OpResult r = MakeR(L"Affinity потоку", t.pid, t.tid);
        DWORD err = 0;
        ProcessInfo pi; pi.pid = t.pid; pi.createTime = t.procCt;
        ULONGLONG pm = 0, sm = 0;
        if (!GetProcAffinity(pi, pm, sm, err)) return Fail(r, err);
        if (mask == 0 || (mask & ~pm)) {
            r.err = ERROR_INVALID_PARAMETER; r.note = L"маска потоку має бути непорожньою підмножиною маски процесу"; return r;
        }
        HANDLE h = OpenThr(t.tid, t.pid, t.createTime, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, err);
        if (!h) return Fail(r, err);
        GROUP_AFFINITY ga;
        if (GetThreadGroupAffinity(h, &ga)) r.oldV = MaskText(ga.Mask);
        if (SetThreadAffinityMask(h, (DWORD_PTR)mask) == 0) { r.err = GetLastError(); CloseHandle(h); return r; }
        if (GetThreadGroupAffinity(h, &ga)) r.newV = MaskText(ga.Mask);
        r.ok = true;
        CloseHandle(h);
        return r;
    }

    OpResult Monitor::SuspendThreadOp(const ThreadInfo& t) {
        OpResult r = MakeR(L"Призупинити потік", t.pid, t.tid);
        if (t.pid == selfPid_ || t.pid == 0 || t.pid == 4) { r.err = ERROR_ACCESS_DENIED; r.note = L"заборонено для цього процесу"; return r; }
        Key pk{ t.pid, t.procCt }, tk{ t.tid, t.createTime };
        r.oldV = L"Активний";
        if (susp_[pk].count(tk)) {
            r.ok = true; r.oldV = r.newV = L"Призупинено цим диспетчером";
            r.note = L"уже призупинено; лічильник не збільшено";
            return r;
        }
        DWORD err = 0;
        HANDLE h = OpenThr(t.tid, t.pid, t.createTime, THREAD_SUSPEND_RESUME, err);
        if (!h) { if (susp_[pk].empty()) susp_.erase(pk); return Fail(r, err); }
        DWORD prev = SuspendThread(h);
        if (prev == (DWORD)-1) { r.err = GetLastError(); CloseHandle(h); if (susp_[pk].empty()) susp_.erase(pk); return r; }
        susp_[pk][tk] = true;
        r.newV = L"Призупинено цим диспетчером";
        r.note = L"попередній suspend count = " + std::to_wstring(prev);
        r.ok = true;
        CloseHandle(h);
        return r;
    }

    OpResult Monitor::ResumeThreadOp(const ThreadInfo& t) {
        OpResult r = MakeR(L"Відновити потік", t.pid, t.tid);
        Key pk{ t.pid, t.procCt }, tk{ t.tid, t.createTime };
        auto it = susp_.find(pk);
        if (it == susp_.end() || !it->second.count(tk)) {
            r.err = ERROR_INVALID_STATE; r.note = L"потік не призупинявся цим диспетчером"; return r;
        }
        DWORD err = 0;
        HANDLE h = OpenThr(t.tid, t.pid, t.createTime, THREAD_SUSPEND_RESUME, err);
        if (!h) return Fail(r, err);
        r.oldV = L"Призупинено цим диспетчером";
        DWORD prev = ResumeThread(h);          // рівно одне зняття власного призупинення
        if (prev == (DWORD)-1) { r.err = GetLastError(); CloseHandle(h); return r; }
        it->second.erase(tk);
        if (it->second.empty()) susp_.erase(it);
        r.newV = L"Активний";
        if (prev > 1) r.note = L"потік усе ще призупинений іншими (лічильник " + std::to_wstring(prev - 1) + L")";
        r.ok = true;
        CloseHandle(h);
        return r;
    }

    OpResult Monitor::SuspendProcess(const ProcessInfo& p) {
        OpResult r = MakeR(L"Призупинити процес", p.pid, 0);
        if (p.pid == selfPid_ || p.pid == 0 || p.pid == 4) { r.err = ERROR_ACCESS_DENIED; r.note = L"заборонено для цього процесу"; return r; }
        DWORD err = 0;
        HANDLE hp = OpenProc(p.pid, p.createTime, 0, err);
        if (!hp) return Fail(r, err);
        CloseHandle(hp);
        Key pk{ p.pid, p.createTime };
        std::map<Key, bool>& sp = susp_[pk];
        DWORD lastErr = 0;
        int okCount = 0;
        for (int attempt = 0; attempt < 3; attempt++) {     // повторна перевірка переліку (операція не атомарна)
            std::map<DWORD, std::vector<TE>> all = SnapThreads();
            std::vector<TE> th = all[p.pid];
            int newly = 0;
            for (size_t i = 0; i < th.size(); i++) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION, FALSE, th[i].tid);
                if (!h) { lastErr = GetLastError(); continue; }
                FILETIME c, e, k, u;
                if (!GetThreadTimes(h, &c, &e, &k, &u)) { lastErr = GetLastError(); CloseHandle(h); continue; }
                Key tk{ th[i].tid, FT(c) };
                if (sp.count(tk)) { CloseHandle(h); continue; }
                DWORD prev = SuspendThread(h);
                if (prev == (DWORD)-1) lastErr = GetLastError();
                else { sp[tk] = true; okCount++; newly++; }
                CloseHandle(h);
            }
            if (newly == 0) break;
        }
        std::vector<TE> fin = SnapThreads()[p.pid];
        size_t covered = 0;
        for (size_t i = 0; i < fin.size(); i++)
            for (auto q = sp.begin(); q != sp.end(); ++q) if (q->first.id == fin[i].tid) { covered++; break; }
        r.oldV = L"Активний";
        if (covered == 0) {
            if (sp.empty()) susp_.erase(pk);
            r.err = lastErr ? lastErr : ERROR_ACCESS_DENIED;
            return r;
        }
        r.ok = true;
        r.newV = (covered == fin.size()) ? L"Призупинено цим диспетчером" : L"Частково призупинено";
        r.note = std::to_wstring(covered) + L" з " + std::to_wstring(fin.size()) + L" потоків";
        if (covered != fin.size() && lastErr) r.err = lastErr;
        return r;
    }

    OpResult Monitor::ResumeProcess(const ProcessInfo& p) {
        OpResult r = MakeR(L"Відновити процес", p.pid, 0);
        Key pk{ p.pid, p.createTime };
        auto it = susp_.find(pk);
        if (it == susp_.end() || it->second.empty()) {
            r.err = ERROR_INVALID_STATE; r.note = L"немає потоків, призупинених цим диспетчером"; return r;
        }
        std::map<Key, bool> copy = it->second;
        int okc = 0, bad = 0; DWORD lastErr = 0;
        for (auto q = copy.begin(); q != copy.end(); ++q) {
            DWORD err = 0;
            HANDLE h = OpenThr(q->first.id, p.pid, q->first.ct, THREAD_SUSPEND_RESUME, err);
            if (!h) {
                if (err == ERROR_NOT_FOUND) { susp_[pk].erase(q->first); continue; }  // потік уже зник
                bad++; lastErr = err; continue;
            }
            DWORD prev = ResumeThread(h);
            if (prev == (DWORD)-1) { bad++; lastErr = GetLastError(); }
            else { okc++; susp_[pk].erase(q->first); }
            CloseHandle(h);
        }
        if (susp_[pk].empty()) susp_.erase(pk);
        r.oldV = L"Призупинено цим диспетчером";
        r.newV = bad ? L"Частково призупинено" : L"Активний";
        r.note = L"відновлено потоків: " + std::to_wstring(okc);
        r.ok = (bad == 0);
        r.err = bad ? lastErr : 0;
        return r;
    }

    OpResult Monitor::Terminate(const ProcessInfo& p) {
        OpResult r = MakeR(L"Завершити процес (TerminateProcess)", p.pid, 0);
        if (p.pid == selfPid_ || p.pid == 0 || p.pid == 4) { r.err = ERROR_ACCESS_DENIED; r.note = L"заборонено для цього процесу"; return r; }
        DWORD err = 0;
        HANDLE h = OpenProc(p.pid, p.createTime, PROCESS_TERMINATE | SYNCHRONIZE, err);
        if (!h) return Fail(r, err);
        r.oldV = L"Активний";
        if (!TerminateProcess(h, 1)) { r.err = GetLastError(); CloseHandle(h); return r; }
        Key k{ p.pid, p.createTime };
        if (tracked_.count(k)) CloseHandle(h);
        else { Tracked t; t.h = h; t.pid = p.pid; t.ct = p.createTime; t.name = p.name; tracked_[k] = t; }
        r.newV = L"запит надіслано";
        r.note = L"фактичне завершення підтвердить сигналізація HANDLE";
        r.ok = true;
        return r;
    }

    OpResult Monitor::TerminateThreadOp(const ThreadInfo& t) {
        OpResult r = MakeR(L"Завершити потік (TerminateThread)", t.pid, t.tid);
        if (!own_.count(Key{ t.pid, t.procCt })) {
            r.err = ERROR_ACCESS_DENIED; r.note = L"дозволено лише для процесів, запущених цим диспетчером"; return r;
        }
        DWORD err = 0;
        HANDLE h = OpenThr(t.tid, t.pid, t.createTime, THREAD_TERMINATE, err);
        if (!h) return Fail(r, err);
        r.oldV = L"Активний";
        if (!TerminateThread(h, 1)) { r.err = GetLastError(); CloseHandle(h); return r; }
        r.newV = L"Завершено (код 1)";
        r.ok = true;
        CloseHandle(h);
        return r;
    }

    OpResult Monitor::Launch(const std::wstring& exe, const std::wstring& args, const std::wstring& dir,
        bool suspended, DWORD& newPid) {
        OpResult r = MakeR(L"CreateProcessW", 0, 0);
        newPid = 0;
        std::wstring cmd = L"\"" + exe + L"\"";
        if (!args.empty()) cmd += L" " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());     // змінюваний буфер
        buf.push_back(0);
        STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        DWORD flags = CREATE_NEW_CONSOLE  | (suspended ? CREATE_SUSPENDED : 0);
        BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, flags, nullptr,
            dir.empty() ? nullptr : dir.c_str(), &si, &pi);
        if (!ok) { r.err = GetLastError(); r.newV = L"процес не створено"; return r; }
        r.pid = pi.dwProcessId; r.tid = suspended ? pi.dwThreadId : 0;
        newPid = pi.dwProcessId;
        FILETIME c, e, k, u;
        ULONGLONG pct = 0;
        if (GetProcessTimes(pi.hProcess, &c, &e, &k, &u)) pct = FT(c);
        Key pk{ pi.dwProcessId, pct };
        own_.insert(pk);
        std::wstring nm = exe;
        size_t sl = nm.find_last_of(L"\\/");
        if (sl != std::wstring::npos) nm = nm.substr(sl + 1);
        Tracked tr; tr.h = pi.hProcess; tr.pid = pi.dwProcessId; tr.ct = pct; tr.name = nm;
        tracked_[pk] = tr;
        if (suspended && GetThreadTimes(pi.hThread, &c, &e, &k, &u)) susp_[pk][Key{ pi.dwThreadId, FT(c) }] = true;
        CloseHandle(pi.hThread);
        r.newV = suspended ? L"створено призупиненим" : L"запущено";
        r.ok = true;
        return r;
    }

    // ---------- Сортування ----------
    static int PriRank(DWORD c) {
        switch (c) {
        case IDLE_PRIORITY_CLASS: return 0;
        case BELOW_NORMAL_PRIORITY_CLASS: return 1;
        case NORMAL_PRIORITY_CLASS: return 2;
        case ABOVE_NORMAL_PRIORITY_CLASS: return 3;
        case HIGH_PRIORITY_CLASS: return 4;
        case REALTIME_PRIORITY_CLASS: return 5;
        }
        return -1;
    }
    static int CmpText(const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) - CSTR_EQUAL;
    }
    template <class T> static int CmpNum(T a, T b) { return a < b ? -1 : (a > b ? 1 : 0); }

    void SortProcs(std::vector<ProcessInfo>& v, int col, bool asc) {
        std::stable_sort(v.begin(), v.end(), [col, asc](const ProcessInfo& a, const ProcessInfo& b) {
            int c = 0;
            switch (col) {
            case 0: c = CmpNum(a.pid, b.pid); break;
            case 1: c = CmpText(a.name, b.name); break;
            case 2: c = CmpNum(a.cpu, b.cpu); break;
            case 3: c = CmpNum(PriRank(a.priorityClass), PriRank(b.priorityClass)); break;
            case 4: c = CmpText(a.state, b.state); break;
            case 5: c = CmpNum(a.ws, b.ws); break;
            case 6: c = CmpNum(a.va, b.va); break;
            case 7: c = CmpNum(a.priv, b.priv); break;
            case 8: c = CmpNum(a.threads, b.threads); break;
            case 9: c = CmpText(a.owner, b.owner); break;
            }
            return asc ? c < 0 : c > 0;
            });
    }

    void SortThreads(std::vector<ThreadInfo>& v, int col, bool asc) {
        std::stable_sort(v.begin(), v.end(), [col, asc](const ThreadInfo& a, const ThreadInfo& b) {
            int c = 0;
            switch (col) {
            case 0: c = CmpNum(a.tid, b.tid); break;
            case 1: c = CmpNum(a.pid, b.pid); break;
            case 2: c = CmpNum(a.cpu, b.cpu); break;
            case 3: c = CmpNum(a.relPriority, b.relPriority); break;
            case 4: c = CmpText(a.state, b.state); break;
            case 5: c = CmpNum(a.basePri, b.basePri); break;
            case 6: c = CmpNum(a.cpuTime100ns, b.cpuTime100ns); break;
            case 7: c = CmpNum(a.group, b.group); break;
            case 8: c = CmpNum(a.affinity, b.affinity); break;
            }
            return asc ? c < 0 : c > 0;
            });
    }

} // namespace ProcApi
