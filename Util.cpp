#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "Util.hpp"

BOOL WritePrivateProfileInt(LPCTSTR lpAppName, LPCTSTR lpKeyName, int value, LPCTSTR lpFileName)
{
    TCHAR sz[16];
    _stprintf_s(sz, TEXT("%d"), value);
    return WritePrivateProfileString(lpAppName, lpKeyName, sz, lpFileName);
}

DWORD GetLongModuleFileName(HMODULE hModule, LPTSTR lpFileName, DWORD nSize)
{
    TCHAR longOrShortName[MAX_PATH];
    DWORD nRet = GetModuleFileName(hModule, longOrShortName, MAX_PATH);
    if (nRet && nRet < MAX_PATH) {
        nRet = GetLongPathName(longOrShortName, lpFileName, nSize);
        if (nRet < nSize) return nRet;
    }
    return 0;
}

bool SyncAdtsPayload(std::vector<BYTE> &workspace, const BYTE *payload, size_t lenBytes)
{
    if (!workspace.empty() && workspace[0] == 0) {
        // No need to resync
        workspace.insert(workspace.end(), payload, payload + lenBytes);
        workspace[0] = 0xff;
    }
    else {
        // Resync
        workspace.insert(workspace.end(), payload, payload + lenBytes);
        size_t i = 0;
        for (; i < workspace.size(); ++i) {
            if (workspace[i] == 0xff && (i + 1 >= workspace.size() || (workspace[i + 1] & 0xf0) == 0xf0)) {
                break;
            }
        }
        workspace.erase(workspace.begin(), workspace.begin() + i);
        if (workspace.size() < 2) {
            return false;
        }
    }
    return true;
}

int get_ts_payload_size(const unsigned char *packet)
{
    int adaptation = extract_ts_header_adaptation(packet);
    if (adaptation & 1) {
        if (adaptation == 3) {
            int adaptation_length = packet[4];
            if (adaptation_length <= 183) {
                return 183 - adaptation_length;
            }
        }
        else {
            return 184;
        }
    }
    return 0;
}
