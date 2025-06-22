
#include <afx.h>
#include <psapi.h>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <string>

#pragma comment(lib, "psapi.lib")

// ---------------- Timer ----------------

class Timer {
public:
    Timer(const CString& label) : m_label(label), m_start(std::chrono::high_resolution_clock::now()) {}

    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - m_start).count();
        TRACE(_T("%s took %lld ms\n"), m_label, duration);
    }

private:
    CString m_label;
    std::chrono::high_resolution_clock::time_point m_start;
};

// ---------------- Memory Logger ----------------

void LogMemoryUsage(const CString& label)
{
    PROCESS_MEMORY_COUNTERS memCounters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memCounters, sizeof(memCounters))) {
        TRACE(_T("%s - WorkingSetSize: %.2f MB, PagefileUsage: %.2f MB\n"),
              label,
              memCounters.WorkingSetSize / (1024.0 * 1024.0),
              memCounters.PagefileUsage / (1024.0 * 1024.0));
    }
}

// ---------------- Mock Record ----------------

class CMyRecord : public CObject {
public:
    CString m_symbol;
    int m_version;
    COleDateTime m_date;

    CMyRecord* Clone() const {
        auto* rec = new CMyRecord();
        rec->m_symbol = m_symbol;
        rec->m_version = m_version;
        rec->m_date = m_date;
        return rec;
    }

    void DoSomething() const {
        volatile int dummy = m_version;
    }

    CString GetSymbol() const { return m_symbol; }
    int GetVersion() const { return m_version; }
    COleDateTime GetDate() const { return m_date; }
};

// ---------------- CacheKey ----------------

struct CacheKey {
    CString Symbol;
    int Version;
    COleDateTime Date;

    bool operator==(const CacheKey& rhs) const {
        return Symbol == rhs.Symbol && Version == rhs.Version && Date == rhs.Date;
    }
};

namespace std {
template <>
struct hash<CacheKey> {
    size_t operator()(const CacheKey& key) const {
        return hash<std::wstring>()((LPCWSTR)key.Symbol) ^
               (hash<int>()(key.Version) << 1) ^
               (hash<__int64>()((__int64)key.Date.m_dt) << 2);
    }
};
}

// ---------------- Test Runner ----------------

std::vector<CMyRecord*> LoadMockRecords(int count)
{
    std::vector<CMyRecord*> result;
    for (int i = 0; i < count; ++i) {
        auto* rec = new CMyRecord();
        rec->m_symbol.Format(_T("SYM%04d"), i % 10);
        rec->m_version = i % 5;
        rec->m_date = COleDateTime::GetCurrentTime();
        result.push_back(rec);
    }
    return result;
}

void RunCacheComparisonTest()
{
    const int kRecordCount = 10000;
    std::vector<CMyRecord*> testRecords = LoadMockRecords(kRecordCount);

    {
        LogMemoryUsage(_T("Before Old Cache"));
        Timer t(_T("Old Cache Method"));
        CArray<CMyRecord*> oldCache;

        for (CMyRecord* record : testRecords) {
            oldCache.Add(record);  // simulate cache fill
        }

        for (int i = 0; i < 1000; ++i) {
            oldCache[i % kRecordCount]->DoSomething();
        }
        LogMemoryUsage(_T("After Old Cache"));
    }

    {
        LogMemoryUsage(_T("Before New Cache"));
        Timer t(_T("New Hash Cache"));

        std::unordered_map<CacheKey, std::unique_ptr<CMyRecord>> newCache;

        for (CMyRecord* record : testRecords) {
            CacheKey key = { record->GetSymbol(), record->GetVersion(), record->GetDate() };
            if (newCache.find(key) == newCache.end()) {
                newCache[key] = std::unique_ptr<CMyRecord>(record->Clone());
            }
        }

        for (int i = 0; i < 1000; ++i) {
            CacheKey key = { _T("SYM0001"), 2, testRecords[i % kRecordCount]->GetDate() };
            auto it = newCache.find(key);
            if (it != newCache.end()) {
                it->second->DoSomething();
            }
        }
        LogMemoryUsage(_T("After New Cache"));
    }

    for (auto* rec : testRecords) delete rec;
}
