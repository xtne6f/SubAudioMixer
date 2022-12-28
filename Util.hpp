#ifndef INCLUDE_UTIL_HPP
#define INCLUDE_UTIL_HPP

#include <mutex>
#include <string>
#include <vector>
#include <tchar.h>

typedef std::lock_guard<std::recursive_mutex> lock_recursive_mutex;

typedef std::basic_string<TCHAR> tstring;

BOOL WritePrivateProfileInt(LPCTSTR lpAppName, LPCTSTR lpKeyName, int value, LPCTSTR lpFileName);
DWORD GetLongModuleFileName(HMODULE hModule, LPTSTR lpFileName, DWORD nSize);

bool SyncAdtsPayload(std::vector<BYTE> &workspace, const BYTE *payload, size_t lenBytes);

int get_ts_payload_size(const unsigned char *packet);

inline int extract_ts_header_error_indicator(const unsigned char *packet) { return !!(packet[1] & 0x80); }
inline int extract_ts_header_unit_start(const unsigned char *packet) { return !!(packet[1] & 0x40); }
inline int extract_ts_header_pid(const unsigned char *packet) { return ((packet[1] & 0x1f) << 8) | packet[2]; }
inline int extract_ts_header_scrambling_control(const unsigned char *packet) { return packet[3] >> 6; }
inline int extract_ts_header_adaptation(const unsigned char *packet) { return (packet[3] >> 4) & 0x03; }
inline int extract_ts_header_counter(const unsigned char *packet) { return packet[3] & 0x0f; }

#endif // INCLUDE_UTIL_HPP
