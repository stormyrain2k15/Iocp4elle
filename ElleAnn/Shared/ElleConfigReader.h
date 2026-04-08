#pragma once
// =============================================================================
// ElleConfigReader.h — Runtime config file reader
//
// Reads C:\Elle\ElleAnn.config at startup and makes the values available
// before the SQL pool is initialized. This is the only place that reads
// the file — call ElleConfigReader::Get().Load() once in OnStart() before
// InitSharedInfrastructure(), then query values with Get().Value(key, default).
//
// Format: KEY=VALUE, one per line, # for comments, blank lines ignored.
// =============================================================================

#include "ElleConfig.h"
#include <string>
#include <unordered_map>
#include <fstream>
#include <Windows.h>

class ElleConfigReader
{
public:
    static ElleConfigReader& Get()
    {
        static ElleConfigReader instance;
        return instance;
    }

    // Load the config file. Silent on failure — defaults from ElleConfig.h apply.
    void Load(const std::wstring& path = ElleConfig::CONFIG_FILE_PATH)
    {
        std::wifstream f(path);
        if (!f.is_open())
        {
            OutputDebugStringW(L"[ElleConfigReader] Config file not found — using compiled defaults\n");
            return;
        }

        std::wstring line;
        while (std::getline(f, line))
        {
            // Strip CR
            if (!line.empty() && line.back() == L'\r')
                line.pop_back();

            // Skip blank lines and comments
            if (line.empty() || line[0] == L'#')
                continue;

            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
                continue;

            std::wstring key = line.substr(0, eq);
            std::wstring val = line.substr(eq + 1);

            // Trim whitespace from both ends
            auto trim = [](std::wstring& s) {
                while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
                while (!s.empty() && iswspace(s.back()))  s.pop_back();
            };
            trim(key);
            trim(val);

            if (!key.empty())
                m_Values[key] = val;
        }

        wchar_t msg[256];
        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
            L"[ElleConfigReader] Loaded %zu entries from %s\n",
            m_Values.size(), path.c_str());
        OutputDebugStringW(msg);
    }

    // Return the value for a key, or defaultVal if the key isn't in the file.
    std::wstring Value(const std::wstring& key, const std::wstring& defaultVal = L"") const
    {
        auto it = m_Values.find(key);
        return (it != m_Values.end()) ? it->second : defaultVal;
    }

    // Build the ODBC connection string for a given database name,
    // using whatever DB_SERVER / DB_UID / DB_PWD are set in the config file.
    std::wstring BuildConnStr(const std::wstring& databaseName) const
    {
        std::wstring server = Value(L"DB_SERVER", ElleConfig::DB::SERVER);
        std::wstring driver = Value(L"DB_DRIVER", ElleConfig::DB::DRIVER);
        std::wstring uid    = Value(L"DB_UID",    ElleConfig::DB::DB_UID);
        std::wstring pwd    = Value(L"DB_PWD",    ElleConfig::DB::DB_PWD);
        std::wstring auth   = Value(L"DB_AUTH",   L"windows");

        wchar_t buf[1024] = {};
        if (!uid.empty() && auth != L"windows")
        {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"Driver=%s;Server=%s;Database=%s;UID=%s;PWD=%s;",
                driver.c_str(), server.c_str(), databaseName.c_str(),
                uid.c_str(), pwd.c_str());
        }
        else
        {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"Driver=%s;Server=%s;Database=%s;Trusted_Connection=Yes;",
                driver.c_str(), server.c_str(), databaseName.c_str());
        }
        return std::wstring(buf);
    }

    // The server string (used by ElleSQLPool::Init)
    std::wstring Server() const
    {
        return Value(L"DB_SERVER", ElleConfig::DB::SERVER);
    }

private:
    ElleConfigReader() = default;
    std::unordered_map<std::wstring, std::wstring> m_Values;
};
