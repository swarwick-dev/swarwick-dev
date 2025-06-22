
#include <afx.h>
#include <psapi.h>
#include <chrono>
#include <unordered_map>
#include <random>
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

// ---------------- Clone Interface ----------------
class ICopyable {
public:
    virtual ICopyable* Clone() const = 0;
    virtual void CopyFrom(const ICopyable* other) = 0;
    virtual ~ICopyable() {}
};

// ---------------- CacheEntry Wrapper ----------------
class ICacheEntry {
public:
    virtual ~ICacheEntry() {}
    virtual CObject* GetObject() = 0;
    virtual ICopyable* GetCopyable() = 0;
    virtual ICacheEntry* Clone() const = 0;
};

class CloneableCacheEntry : public ICacheEntry {
    std::unique_ptr<ICopyable> m_copyable;
    CObject* m_object;
public:
    CloneableCacheEntry(std::unique_ptr<ICopyable> obj)
        : m_object(dynamic_cast<CObject*>(obj.get())), m_copyable(std::move(obj)) {}

    CObject* GetObject() override { return m_object; }
    ICopyable* GetCopyable() override { return m_copyable.get(); }
    ICacheEntry* Clone() const override {
        return new CloneableCacheEntry(std::unique_ptr<ICopyable>(m_copyable->Clone()));
    }
};

// ---------------- CTblAudit and CAuditEntry ----------------
class CAuditEntry : public CObject, public ICopyable {
public:
    int m_id;
    CString m_detail;

    ICopyable* Clone() const override {
        auto* entry = new CAuditEntry();
        entry->m_id = m_id;
        entry->m_detail = m_detail;
        return entry;
    }

    void CopyFrom(const ICopyable* other) override {
        const CAuditEntry* src = dynamic_cast<const CAuditEntry*>(other);
        if (src) {
            m_id = src->m_id;
            m_detail = src->m_detail;
        }
    }
};

class CTblAudit : public CObject, public ICopyable {
public:
    CString m_key;
    CArray<CAuditEntry*> m_entries;

    ~CTblAudit() {
        for (int i = 0; i < m_entries.GetSize(); ++i)
            delete m_entries[i];
    }

    ICopyable* Clone() const override {
        auto* tbl = new CTblAudit();
        tbl->m_key = m_key;
        for (int i = 0; i < m_entries.GetSize(); ++i) {
            tbl->m_entries.Add(static_cast<CAuditEntry*>(m_entries[i]->Clone()));
        }
        return tbl;
    }

    void CopyFrom(const ICopyable* other) override {
        const CTblAudit* src = dynamic_cast<const CTblAudit*>(other);
        if (src) {
            m_key = src->m_key;
            for (int i = 0; i < src->m_entries.GetSize(); ++i) {
                m_entries.Add(static_cast<CAuditEntry*>(src->m_entries[i]->Clone()));
            }
        }
    }
};

// ---------------- Cache Key ----------------
struct AuditCacheKey {
    CString key;

    bool operator==(const AuditCacheKey& other) const {
        return key == other.key;
    }
};

namespace std {
template <>
struct hash<AuditCacheKey> {
    size_t operator()(const AuditCacheKey& k) const {
        return hash<std::wstring>()((LPCWSTR)k.key);
    }
};
}

// ---------------- Unified Cache Bridge ----------------
class UnifiedCacheBridge {
public:
    void AddToCache(const CString& key, CObject* legacyObj, std::unique_ptr<ICacheEntry> modernEntry) {
        m_legacyCache[key] = legacyObj;
        m_modernCache[{ key }] = std::move(modernEntry);
    }

    bool Lookup(const CString& key, CObject*& outObj) const {
        auto it = m_legacyCache.find(key);
        if (it != m_legacyCache.end()) {
            outObj = it->second;
            return true;
        }
        auto mit = m_modernCache.find({ key });
        if (mit != m_modernCache.end()) {
            outObj = mit->second->GetObject();
            return true;
        }
        return false;
    }

    size_t Size() const { return m_modernCache.size(); }

private:
    std::unordered_map<CString, CObject*> m_legacyCache;
    std::unordered_map<AuditCacheKey, std::unique_ptr<ICacheEntry>> m_modernCache;
};

// ---------------- Test ----------------
void RunUnifiedAuditCacheTest() {
    const int totalRecords = 5000;
    const int entriesPerRecord = 10;

    UnifiedCacheBridge bridge;

    {
        Timer t(_T("Insert into Unified Cache"));
        for (int i = 0; i < totalRecords; ++i) {
            auto* record = new CTblAudit();
            record->m_key.Format(_T("AUDIT_%05d"), i);
            for (int j = 0; j < entriesPerRecord; ++j) {
                auto* entry = new CAuditEntry();
                entry->m_id = j;
                entry->m_detail.Format(_T("Detail %d"), j);
                record->m_entries.Add(entry);
            }
            bridge.AddToCache(record->m_key, record, std::make_unique<CloneableCacheEntry>(std::unique_ptr<ICopyable>(record->Clone())));
        }
    }

    {
        Timer t(_T("Random Read Test from Unified Cache"));
        std::default_random_engine rng(42);
        std::uniform_int_distribution<int> dist(0, totalRecords - 1);

        for (int i = 0; i < 10000; ++i) {
            CString key;
            key.Format(_T("AUDIT_%05d"), dist(rng));
            CObject* obj = nullptr;
            if (bridge.Lookup(key, obj)) {
                CTblAudit* audit = dynamic_cast<CTblAudit*>(obj);
                if (audit && audit->m_entries.GetSize() > 0) {
                    audit->m_entries[0]->m_detail.GetLength(); // simulate read
                }
            }
        }
    }

    LogMemoryUsage(_T("After Unified Cache Test"));
}


// ---------------- Serialization Support ----------------
void SerializeAuditEntry(CArchive& ar, CAuditEntry* entry) {
    if (ar.IsStoring()) {
        ar << entry->m_id;
        ar << entry->m_detail;
    } else {
        ar >> entry->m_id;
        ar >> entry->m_detail;
    }
}

void SerializeAuditTable(CArchive& ar, CTblAudit* audit) {
    if (ar.IsStoring()) {
        ar << audit->m_key;
        int count = audit->m_entries.GetSize();
        ar << count;
        for (int i = 0; i < count; ++i)
            SerializeAuditEntry(ar, audit->m_entries[i]);
    } else {
        ar >> audit->m_key;
        int count;
        ar >> count;
        for (int i = 0; i < count; ++i) {
            auto* entry = new CAuditEntry();
            SerializeAuditEntry(ar, entry);
            audit->m_entries.Add(entry);
        }
    }
}

// ---------------- Save/Load Wrapper ----------------
void SaveAuditCacheToFile(const CString& path, const UnifiedCacheBridge& bridge) {
    CFile file(path, CFile::modeCreate | CFile::modeWrite);
    CArchive ar(&file, CArchive::store);

    ar << (int)bridge.Size();
    for (int i = 0; i < bridge.Size(); ++i) {
        CString key;
        key.Format(_T("AUDIT_%05d"), i); // Key must match insert pattern
        CObject* obj = nullptr;
        if (bridge.Lookup(key, obj)) {
            CTblAudit* audit = dynamic_cast<CTblAudit*>(obj);
            if (audit) {
                SerializeAuditTable(ar, audit);
            }
        }
    }
    ar.Close();
    file.Close();
}

void LoadAuditCacheFromFile(const CString& path, UnifiedCacheBridge& bridge) {
    CFile file(path, CFile::modeRead);
    CArchive ar(&file, CArchive::load);

    int count;
    ar >> count;
    for (int i = 0; i < count; ++i) {
        auto* audit = new CTblAudit();
        SerializeAuditTable(ar, audit);
        bridge.AddToCache(audit->m_key, audit, std::make_unique<CloneableCacheEntry>(std::unique_ptr<ICopyable>(audit->Clone())));
    }

    ar.Close();
    file.Close();
}

// ---------------- Updated Main ----------------
void RunUnifiedAuditCacheTestWithSaveLoad() {
    CString filePath = _T("AuditCacheTestOutput.bin");

    {
        Timer t(_T("Writing Cache to File"));
        UnifiedCacheBridge bridge;
        for (int i = 0; i < 5000; ++i) {
            auto* record = new CTblAudit();
            record->m_key.Format(_T("AUDIT_%05d"), i);
            for (int j = 0; j < 10; ++j) {
                auto* entry = new CAuditEntry();
                entry->m_id = j;
                entry->m_detail.Format(_T("Detail %d"), j);
                record->m_entries.Add(entry);
            }
            bridge.AddToCache(record->m_key, record, std::make_unique<CloneableCacheEntry>(std::unique_ptr<ICopyable>(record->Clone())));
        }
        SaveAuditCacheToFile(filePath, bridge);
    }

    UnifiedCacheBridge loadBridge;
    {
        Timer t(_T("Reading Cache from File"));
        LoadAuditCacheFromFile(filePath, loadBridge);
    }

    {
        Timer t(_T("Random Access Test from Loaded Cache"));
        std::default_random_engine rng(42);
        std::uniform_int_distribution<int> dist(0, 4999);

        for (int i = 0; i < 10000; ++i) {
            CString key;
            key.Format(_T("AUDIT_%05d"), dist(rng));
            CObject* obj = nullptr;
            if (loadBridge.Lookup(key, obj)) {
                CTblAudit* audit = dynamic_cast<CTblAudit*>(obj);
                if (audit && audit->m_entries.GetSize() > 0) {
                    audit->m_entries[0]->m_detail.GetLength(); // simulate read
                }
            }
        }
    }

    LogMemoryUsage(_T("After Reload and Access Test"));
}


// ---------------- JSON-like Dump Support ----------------
CString JsonEscape(const CString& input) {
    CString result;
    for (int i = 0; i < input.GetLength(); ++i) {
        switch (input[i]) {
        case _T('"'): result += _T("\\\""); break;
        case _T('\\'): result += _T("\\\\"); break;
        case _T('\b'): result += _T("\\b"); break;
        case _T('\f'): result += _T("\\f"); break;
        case _T('\n'): result += _T("\\n"); break;
        case _T('\r'): result += _T("\\r"); break;
        case _T('\t'): result += _T("\\t"); break;
        default:
            if (input[i] < 32 || input[i] > 126)
                result.AppendFormat(_T("\\u%04x"), input[i]);
            else
                result += input[i];
            break;
        }
    }
    return result;
}

CString DumpAuditEntryToJson(const CAuditEntry* entry) {
    CString result;
    result.AppendFormat(_T("{ \"id\": %d, \"detail\": \"%s\" }"),
                        entry->m_id, JsonEscape(entry->m_detail));
    return result;
}

CString DumpAuditTableToJson(const CTblAudit* audit) {
    CString result = _T("{\n");
    result.AppendFormat(_T("  \"key\": \"%s\",\n"), JsonEscape(audit->m_key));
    result += _T("  \"entries\": [\n");

    for (int i = 0; i < audit->m_entries.GetSize(); ++i) {
        result += _T("    ") + DumpAuditEntryToJson(audit->m_entries[i]);
        if (i != audit->m_entries.GetSize() - 1) result += _T(",");
        result += _T("\n");
    }

    result += _T("  ]\n}");
    return result;
}

// ---------------- Manual Dump Usage Example ----------------
void DumpRandomAuditRecordJson(const UnifiedCacheBridge& bridge) {
    CString key = _T("AUDIT_00042");
    CObject* obj = nullptr;
    if (bridge.Lookup(key, obj)) {
        CTblAudit* audit = dynamic_cast<CTblAudit*>(obj);
        if (audit) {
            CString json = DumpAuditTableToJson(audit);
            TRACE(_T("JSON for %s:\n%s\n"), key, json);
        }
    }
}
