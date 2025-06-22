
#include <afx.h>
#include <psapi.h>
#include <chrono>
#include <unordered_map>
#include <iostream>
#include <memory>
#include <vector>

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

// ---------------- Memory Usage ----------------
void LogMemoryUsage(const CString& label) {
    PROCESS_MEMORY_COUNTERS memCounters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memCounters, sizeof(memCounters))) {
        TRACE(_T("%s - WorkingSetSize: %.2f MB, PagefileUsage: %.2f MB\n"),
              label,
              memCounters.WorkingSetSize / (1024.0 * 1024.0),
              memCounters.PagefileUsage / (1024.0 * 1024.0));
    }
}

// ---------------- Base Interfaces ----------------
class ICopyable {
public:
    virtual ICopyable* Clone() const = 0;
    virtual void CopyFrom(const ICopyable* other) = 0;
    virtual ~ICopyable() {}
};

class ICacheEntry {
public:
    virtual ~ICacheEntry() {}
    virtual CObject* GetObject() = 0;
    virtual ICopyable* GetCopyable() = 0;
    virtual ICacheEntry* Clone() const = 0;
};

// ---------------- Record ----------------
class CMyRecord : public CObject, public ICopyable {
public:
    CString m_symbol;
    int m_version;
    COleDateTime m_date;

    CMyRecord() {}

    CMyRecord(const CMyRecord& other) {
        CopyFrom(&other);
    }

    void CopyFrom(const ICopyable* other) override {
        const CMyRecord* p = dynamic_cast<const CMyRecord*>(other);
        if (p) {
            m_symbol = p->m_symbol;
            m_version = p->m_version;
            m_date = p->m_date;
        }
    }

    ICopyable* Clone() const override {
        return new CMyRecord(*this);
    }

    void DoSomething() const {
        volatile int dummy = m_version;
    }

    CString GetSymbol() const { return m_symbol; }
    int GetVersion() const { return m_version; }
    COleDateTime GetDate() const { return m_date; }
};

// ---------------- Cache Wrappers ----------------
class LegacyCacheEntry : public ICacheEntry {
    CObject* m_obj;

public:
    LegacyCacheEntry(CObject* obj) : m_obj(obj) {}

    CObject* GetObject() override { return m_obj; }
    ICopyable* GetCopyable() override { return nullptr; }

    ICacheEntry* Clone() const override {
        CMemFile mem;
        CArchive arWrite(&mem, CArchive::store);
        m_obj->Serialize(arWrite);
        arWrite.Close();

        UINT len = (UINT)mem.GetLength();
        BYTE* buffer = mem.Detach();

        CMemFile memRead(buffer, len);
        CArchive arRead(&memRead, CArchive::load);

        auto* copy = static_cast<CObject*>(m_obj->GetRuntimeClass()->CreateObject());
        copy->Serialize(arRead);
        arRead.Close();
        delete[] buffer;

        return new LegacyCacheEntry(copy);
    }
};

class CloneableCacheEntry : public ICacheEntry {
    std::unique_ptr<ICopyable> m_copyable;

public:
    CloneableCacheEntry(std::unique_ptr<ICopyable> obj) : m_copyable(std::move(obj)) {}

    CObject* GetObject() override {
        return dynamic_cast<CObject*>(m_copyable.get());
    }

    ICopyable* GetCopyable() override {
        return m_copyable.get();
    }

    ICacheEntry* Clone() const override {
        return new CloneableCacheEntry(std::unique_ptr<ICopyable>(m_copyable->Clone()));
    }
};

// ---------------- Key Struct ----------------
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

// ---------------- Factory Helper ----------------
std::unique_ptr<ICacheEntry> WrapForCache(CObject* obj) {
    if (auto* copyable = dynamic_cast<ICopyable*>(obj)) {
        return std::make_unique<CloneableCacheEntry>(std::unique_ptr<ICopyable>(copyable->Clone()));
    } else {
        return std::make_unique<LegacyCacheEntry>(obj);
    }
}

// ---------------- Mock Record Generator ----------------
std::vector<CMyRecord*> LoadMockRecords(int count) {
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

// ---------------- Test Harness ----------------
void RunUnifiedCacheComparisonTest() {
    const int kRecordCount = 10000;
    std::vector<CMyRecord*> testRecords = LoadMockRecords(kRecordCount);
    std::unordered_map<CacheKey, std::unique_ptr<ICacheEntry>> m_cache;

    {
        LogMemoryUsage(_T("Before Cache Fill"));
        Timer t(_T("Unified Cache Insert"));

        for (CMyRecord* record : testRecords) {
            CacheKey key = { record->GetSymbol(), record->GetVersion(), record->GetDate() };
            m_cache[key] = WrapForCache(record);
        }
        LogMemoryUsage(_T("After Cache Fill"));
    }

    {
        Timer t(_T("Unified Cache Access"));
        for (int i = 0; i < 1000; ++i) {
            CacheKey key = { _T("SYM0001"), 2, testRecords[i % kRecordCount]->GetDate() };
            auto it = m_cache.find(key);
            if (it != m_cache.end()) {
                ICacheEntry* entry = it->second.get();
                if (ICopyable* modern = entry->GetCopyable()) {
                    modern->Clone(); // simulate access
                } else {
                    CObject* legacy = entry->GetObject();
                    legacy->IsKindOf(RUNTIME_CLASS(CMyRecord));
                }
            }
        }
    }

    for (auto* rec : testRecords) delete rec;
}
