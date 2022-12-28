// 副音声を合成する
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <limits.h>
#include <neaacdec.h>
#include <algorithm>
#include <utility>
#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#define TVTEST_PLUGIN_VERSION TVTEST_PLUGIN_VERSION_(0,0,14)
#include "TVTestPlugin.h"
#include "Util.hpp"
#include "resource.h"

namespace
{
const TCHAR INFO_PLUGIN_NAME[] = TEXT("SubAudioMixer");
const TCHAR INFO_DESCRIPTION[] = TEXT("副音声を合成する (ver.1.0)");

const LONGLONG PTS_DISCONTINUITY_THRESHOLD_MSEC = 1000;
const DWORD MIN_DELAY_MSEC = 50;
const DWORD MAX_DELAY_MSEC = 5000;
const DWORD MAX_JITTER_MSEC = 3000;
const size_t ERASE_PCM_SIZE = 32 * 1024;
const int AUDIO_CALLBACK_INACTIVE = 1000;
}

class CSubAudioMixer : public TVTest::CTVTestPlugin
{
public:
    CSubAudioMixer()
        : m_fSettingsLoaded(false)
        , m_fEnabled(false)
    {
    }

    bool GetPluginInfo(TVTest::PluginInfo *pInfo)
    {
        pInfo->Type = TVTest::PLUGIN_TYPE_NORMAL;
        pInfo->Flags = TVTest::PLUGIN_FLAG_HASSETTINGS;
        pInfo->pszPluginName = INFO_PLUGIN_NAME;
        pInfo->pszCopyright = L"Public Domain";
        pInfo->pszDescription = INFO_DESCRIPTION;
        return true;
    }

    bool Initialize()
    {
        TCHAR path[MAX_PATH];
        if (!GetLongModuleFileName(g_hinstDLL, path, _countof(path))) return false;
        m_iniPath = path;
        size_t lastSep = m_iniPath.find_last_of(TEXT("/\\."));
        if (lastSep != tstring::npos && m_iniPath[lastSep] == TEXT('.')) {
            m_iniPath.erase(lastSep);
        }
        m_iniPath += TEXT(".ini");

        TVTest::CommandInfo ci;
        ci.ID = 0;
        ci.pszText = L"ShowOptions";
        ci.pszName = L"設定画面を表示";
        m_pApp->RegisterCommand(&ci, 1);

        m_pApp->SetEventCallback(EventCallback, this);
        return true;
    }

    bool Finalize()
    {
        DisablePlugin();
        return true;
    }

    bool EnablePlugin()
    {
        if (!m_fEnabled) {
            LoadSettings();
            for (int i = 0; i < 2; ++i) {
                m_decodeInfo[i].hAACDec = nullptr;
                m_decodeInfo[i].samplerate = 0;
                // 初期化のため
                m_decodeInfo[i].pid = -2;
            }
            UpdateAudioStreamInfo();
            m_audioCallbackInactiveCount = AUDIO_CALLBACK_INACTIVE;
            m_fEnabled = m_pApp->SetAudioCallback(AudioCallback, this);
            if (m_fEnabled) {
                m_pApp->SetStreamCallback(0, StreamCallback, this);
            }
        }
        return m_fEnabled;
    }

    bool DisablePlugin()
    {
        if (m_fEnabled) {
            m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback);
            m_pApp->SetAudioCallback(nullptr);
            for (int i = 0; i < 2; ++i) {
                if (m_decodeInfo[i].hAACDec) {
                    NeAACDecClose(m_decodeInfo[i].hAACDec);
                    m_decodeInfo[i].hAACDec = nullptr;
                }
            }
            m_fEnabled = false;
        }
        return true;
    }

    static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData)
    {
        static_cast<void>(lParam2);
        CSubAudioMixer &this_ = *static_cast<CSubAudioMixer*>(pClientData);

        switch (Event) {
        case TVTest::EVENT_PLUGINENABLE:
            // プラグインの有効状態が変化した
            return lParam1 ? this_.EnablePlugin() : this_.DisablePlugin();
        case TVTest::EVENT_PLUGINSETTINGS:
            // プラグインの設定を行う
            return this_.PluginSettings(reinterpret_cast<HWND>(lParam1));
        case TVTest::EVENT_CHANNELCHANGE:
            // チャンネルが変更された
        case TVTest::EVENT_SERVICECHANGE:
            // サービスが変更された
        case TVTest::EVENT_SERVICEUPDATE:
            // サービスの構成が変化した
        case TVTest::EVENT_STEREOMODECHANGE:
            // ステレオモードが変化した
        case TVTest::EVENT_RESET:
            // リセットされた
        case TVTest::EVENT_AUDIOSTREAMCHANGE:
            // 音声ストリームが変更された
            if (this_.m_fEnabled) {
                this_.UpdateAudioStreamInfo();
            }
            break;
        case TVTest::EVENT_COMMAND:
            // コマンドが選択された
            if (lParam1 == 0) {
                HWND hwndFull = this_.GetFullscreenWindow();
                this_.PluginSettings(hwndFull ? hwndFull : this_.m_pApp->GetAppWindow());
            }
            break;
        }
        return 0;
    }

private:
    void LoadSettings()
    {
        if (!m_fSettingsLoaded) {
            m_audioBalance = GetPrivateProfileInt(TEXT("Settings"), TEXT("AudioBalance"), 50, m_iniPath.c_str());
            m_audioMainBalance = GetPrivateProfileInt(TEXT("Settings"), TEXT("AudioMainBalance"), 0, m_iniPath.c_str());
            m_audioSubBalance = GetPrivateProfileInt(TEXT("Settings"), TEXT("AudioSubBalance"), 100, m_iniPath.c_str());
            m_audioBalance = std::min(std::max(m_audioBalance, 0), 100);
            m_audioMainBalance = std::min(std::max(m_audioMainBalance, 0), 100);
            m_audioSubBalance = std::min(std::max(m_audioSubBalance, 0), 100);
            m_fMonoMain = GetPrivateProfileInt(TEXT("Settings"), TEXT("MonoMain"), 0, m_iniPath.c_str()) != 0;
            m_fMonoSub = GetPrivateProfileInt(TEXT("Settings"), TEXT("MonoSub"), 0, m_iniPath.c_str()) != 0;
            m_delayMsec = GetPrivateProfileInt(TEXT("Settings"), TEXT("DelayMsec"), 150, m_iniPath.c_str());
            m_delayMsec = std::min(std::max(m_delayMsec, MIN_DELAY_MSEC), MAX_DELAY_MSEC);
            m_fSettingsLoaded = true;
        }
    }

    HWND GetFullscreenWindow()
    {
        TVTest::HostInfo hostInfo;
        if (m_pApp->GetFullscreen() && m_pApp->GetHostInfo(&hostInfo)) {
            tstring className = tstring(hostInfo.pszAppName) + TEXT(" Fullscreen");
            HWND hwnd = nullptr;
            while ((hwnd = FindWindowEx(nullptr, hwnd, className.c_str(), nullptr)) != nullptr) {
                DWORD pid;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == GetCurrentProcessId()) return hwnd;
            }
        }
        return nullptr;
    }

    void ResetAudioBuffer()
    {
        m_decodeInfo[0].pcm.clear();
        m_decodeInfo[0].pcmSize = 0;
        m_decodeInfo[1].pcm.clear();
        m_decodeInfo[1].pcmSize = 0;
        m_fReset = true;
    }

    void UpdateAudioStreamInfo()
    {
        int pid[2] = {-1, -1};
        int index = m_pApp->GetService();
        TVTest::ServiceInfo si;
        if (index >= 0 && m_pApp->GetServiceInfo(index, &si)) {
            if (si.NumAudioPIDs >= 1) {
                pid[0] = si.AudioPID[0];
            }
            if (si.NumAudioPIDs >= 2) {
                pid[1] = si.AudioPID[1];
            }
        }
        for (int i = 0; i < 2; ++i) {
            if (m_decodeInfo[i].pid != pid[i]) {
                lock_recursive_mutex lock(m_streamLock);
                m_decodeInfo[i].pid = pid[i];
                m_decodeInfo[i].pes.second.clear();
                m_decodeInfo[i].pts = -1;
                m_decodeInfo[i].workspace.clear();
                ResetAudioBuffer();
            }
        }
    }

    static void ErasePcm(std::vector<short> &pcm, size_t &pcmSize, size_t eraseSize)
    {
        // 毎回消すと高コストなのである程度まとめる
        pcmSize -= eraseSize;
        if (pcm.size() - pcmSize >= ERASE_PCM_SIZE) {
            pcm.erase(pcm.begin(), pcm.end() - pcmSize);
        }
    }

    static LRESULT CALLBACK AudioCallback(short *pData, DWORD Samples, int Channels, void *pClientData)
    {
        CSubAudioMixer &this_ = *static_cast<CSubAudioMixer*>(pClientData);
        DECODE_INFO &dec0 = this_.m_decodeInfo[0];
        DECODE_INFO &dec1 = this_.m_decodeInfo[1];
        lock_recursive_mutex lock(this_.m_streamLock);

        this_.m_audioCallbackInactiveCount = 0;

        if (Channels == 2 && dec0.pid >= 0) {
            if (this_.m_fReset) {
                DWORD delaySamples = this_.m_delayMsec * 48;
                if (dec0.pcmSize >= delaySamples * 2 &&
                    (dec1.pid < 0 || dec1.pcmSize >= delaySamples * 2))
                {
                    ErasePcm(dec0.pcm, dec0.pcmSize, dec0.pcmSize - delaySamples * 2);
                    if (dec1.pid >= 0) {
                        ErasePcm(dec1.pcm, dec1.pcmSize, dec1.pcmSize - delaySamples * 2);
                    }
                    this_.m_fReset = false;
                }
            }
            if (!this_.m_fReset && dec0.pcmSize >= Samples * 2 &&
                (dec1.pid < 0 || dec1.pcmSize >= Samples * 2))
            {
                // 合成の係数を設定。intの範囲で計算するため最大値を2000に調整する
                int m[8];
                m[0] = m[5] = this_.m_fMonoMain ? 1 : 2;
                m[1] = m[4] = this_.m_fMonoMain ? 1 : 0;
                m[2] = m[7] = this_.m_fMonoSub ? 1 : 2;
                m[3] = m[6] = this_.m_fMonoSub ? 1 : 0;
                m[0] *= (100 - this_.m_audioMainBalance) * (100 - this_.m_audioBalance) / 10;
                m[1] *= (100 - this_.m_audioMainBalance) * (100 - this_.m_audioBalance) / 10;
                m[2] *= (100 - this_.m_audioSubBalance) * this_.m_audioBalance / 10;
                m[3] *= (100 - this_.m_audioSubBalance) * this_.m_audioBalance / 10;
                m[4] *= this_.m_audioMainBalance * (100 - this_.m_audioBalance) / 10;
                m[5] *= this_.m_audioMainBalance * (100 - this_.m_audioBalance) / 10;
                m[6] *= this_.m_audioSubBalance * this_.m_audioBalance / 10;
                m[7] *= this_.m_audioSubBalance * this_.m_audioBalance / 10;

                auto itSrc0 = dec0.pcm.end() - dec0.pcmSize;
                auto itSrc1 = dec1.pid < 0 ? itSrc0 : dec1.pcm.end() - dec1.pcmSize;
                for (DWORD i = 0; i < Samples * 2; i += 2) {
                    pData[i + 0] = static_cast<short>(std::min(std::max((*(itSrc0 + i) * m[0] +
                                                                         *(itSrc0 + i + 1) * m[1] +
                                                                         *(itSrc1 + i) * m[2] +
                                                                         *(itSrc1 + i + 1) * m[3]) / 2000, SHRT_MIN), SHRT_MAX));
                    pData[i + 1] = static_cast<short>(std::min(std::max((*(itSrc0 + i) * m[4] +
                                                                         *(itSrc0 + i + 1) * m[5] +
                                                                         *(itSrc1 + i) * m[6] +
                                                                         *(itSrc1 + i + 1) * m[7]) / 2000, SHRT_MIN), SHRT_MAX));
                }
                ErasePcm(dec0.pcm, dec0.pcmSize, Samples * 2);
                if (dec1.pid >= 0) {
                    ErasePcm(dec1.pcm, dec1.pcmSize, Samples * 2);
                }
            }
            else if ((dec0.samplerate == 0 || (dec0.samplerate == 48000 && (dec0.channels == 1 || dec0.channels == 2))) &&
                     (dec1.pid < 0 || (dec1.samplerate == 0 || (dec1.samplerate == 48000 && (dec1.channels == 1 || dec1.channels == 2)))))
            {
                // 一時的に出力できないので無音にする
                std::fill_n(pData, Samples * Channels, static_cast<short>(0));
            }
        }
        else {
            this_.ResetAudioBuffer();
        }
        return 0;
    }

    static BOOL CALLBACK StreamCallback(BYTE *pData, void *pClientData)
    {
        CSubAudioMixer &this_ = *static_cast<CSubAudioMixer*>(pClientData);
        int unitStart = extract_ts_header_unit_start(pData);
        int pid = extract_ts_header_pid(pData);
        int counter = extract_ts_header_counter(pData);
        int payloadSize = get_ts_payload_size(pData);
        const BYTE *pPayload = pData + 188 - payloadSize;

        if (extract_ts_header_error_indicator(pData) ||
            extract_ts_header_scrambling_control(pData))
        {
            return TRUE;
        }
        lock_recursive_mutex lock(this_.m_streamLock);
        if (pid != this_.m_decodeInfo[0].pid && pid != this_.m_decodeInfo[1].pid) {
            return TRUE;
        }

        DECODE_INFO &dec = this_.m_decodeInfo[pid == this_.m_decodeInfo[1].pid];
        int &pesCounter = dec.pes.first;
        std::vector<BYTE> &pes = dec.pes.second;
        if (unitStart) {
            pesCounter = counter;
            pes.assign(pPayload, pPayload + payloadSize);
        }
        else if (!pes.empty()) {
            pesCounter = (pesCounter + 1) & 0x0f;
            if (pesCounter == counter) {
                pes.insert(pes.end(), pPayload, pPayload + payloadSize);
            }
            else {
                // 次のPESヘッダまで無視
                pes.clear();
            }
        }
        if (pes.size() >= 6) {
            size_t pesPacketLength = (pes[4] << 8) | pes[5];
            if (pes.size() >= 6 + pesPacketLength) {
                // PESが蓄積された
                pes.resize(6 + pesPacketLength);
                int streamID = pes[3];
                if (pes[0] == 0 && pes[1] == 0 && pes[2] == 1 && (streamID & 0xe0) == 0xc0 && pes.size() >= 9) {
                    size_t payloadPos = 9 + pes[8];
                    if (payloadPos < pes.size() && SyncAdtsPayload(dec.workspace, pes.data() + payloadPos, pes.size() - payloadPos)) {
                        if ((pes[7] & 0x80) && pes.size() >= 14) {
                            LONGLONG pts = static_cast<LONGLONG>(pes[9] & 0x0e) << 29 |
                                           pes[10] << 22 | (pes[11] & 0xfe) << 14 | pes[12] << 7 | pes[13] >> 1;
                            if (dec.pts < 0 || ((0x200000000 + pts - dec.pts) & 0x1ffffffff) > 90 * PTS_DISCONTINUITY_THRESHOLD_MSEC) {
                                this_.ResetAudioBuffer();
                            }
                            dec.pts = pts;
                        }
                        // ADTSフレーム単位に分解
                        while (dec.workspace.size() > 0) {
                            if (dec.workspace[0] != 0xff) {
                                dec.workspace.clear();
                                break;
                            }
                            if (dec.workspace.size() < 7) {
                                break;
                            }
                            if ((dec.workspace[1] & 0xf0) != 0xf0) {
                                dec.workspace.clear();
                                break;
                            }
                            DWORD frameLenBytes = (dec.workspace[3] & 0x03) << 11 | dec.workspace[4] << 3 | dec.workspace[5] >> 5;
                            if (frameLenBytes < 7) {
                                dec.workspace.clear();
                                break;
                            }
                            if (dec.workspace.size() < frameLenBytes) {
                                break;
                            }

                            if (dec.samplerate == 0) {
                                if (!dec.hAACDec && this_.m_audioCallbackInactiveCount < AUDIO_CALLBACK_INACTIVE) {
                                    dec.hAACDec = NeAACDecOpen();
                                }
                                if (dec.hAACDec &&
                                    NeAACDecInit(dec.hAACDec, dec.workspace.data(), frameLenBytes, &dec.samplerate, &dec.channels) < 0)
                                {
                                    dec.samplerate = 0;
                                }
                            }
                            if (dec.samplerate != 0) {
                                NeAACDecFrameInfo frame;
                                BYTE *pcm = static_cast<BYTE*>(NeAACDecDecode(dec.hAACDec, &frame, dec.workspace.data(), frameLenBytes));
                                if (frame.error || this_.m_audioCallbackInactiveCount >= AUDIO_CALLBACK_INACTIVE) {
                                    dec.samplerate = 0;
                                    this_.ResetAudioBuffer();
                                    NeAACDecClose(dec.hAACDec);
                                    dec.hAACDec = nullptr;
                                }
                                else {
                                    ++this_.m_audioCallbackInactiveCount;
                                    if (dec.samplerate != frame.samplerate || dec.channels != frame.channels) {
                                        dec.samplerate = frame.samplerate;
                                        dec.channels = frame.channels;
                                        this_.ResetAudioBuffer();
                                    }
                                    if (dec.samplerate == 48000) {
                                        if (dec.channels == 1) {
                                            for (size_t i = 0; i < frame.samples; ++i) {
                                                short s = pcm[i * 2] | pcm[i * 2 + 1] << 8;
                                                dec.pcm.push_back(s);
                                                dec.pcm.push_back(s);
                                                dec.pcmSize += 2;
                                            }
                                        }
                                        else if (dec.channels == 2) {
                                            dec.pcm.resize(dec.pcm.size() + frame.samples);
                                            dec.pcmSize += frame.samples;
                                            std::copy(pcm, pcm + frame.samples * 2,
                                                      reinterpret_cast<BYTE*>(dec.pcm.data() + dec.pcm.size()) - frame.samples * 2);
                                        }
                                        if (dec.pcmSize > (this_.m_delayMsec + MAX_JITTER_MSEC) * 48 * 2) {
                                            ErasePcm(dec.pcm, dec.pcmSize, dec.pcmSize - (this_.m_delayMsec + MAX_JITTER_MSEC) * 48 * 2);
                                        }
                                    }
                                }
                            }

                            dec.workspace.erase(dec.workspace.begin(), dec.workspace.begin() + frameLenBytes);
                        }
                        if (!dec.workspace.empty()) {
                            dec.workspace[0] = 0;
                        }
                    }
                }
                pes.clear();
            }
        }
        return TRUE;
    }

    bool PluginSettings(HWND hwndOwner)
    {
        TVTest::ShowDialogInfo info;
        info.Flags = 0;
        info.hinst = g_hinstDLL;
        info.pszTemplate = MAKEINTRESOURCE(IDD_OPTIONS);
        info.pMessageFunc = TVTestSettingsDlgProc;
        info.pClientData = this;
        info.hwndOwner = hwndOwner;
        m_pApp->ShowDialog(&info);
        return true;
    }

    static INT_PTR CALLBACK TVTestSettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void *pClientData)
    {
        static_cast<void>(lParam);
        CSubAudioMixer &this_ = *static_cast<CSubAudioMixer*>(pClientData);

        switch (uMsg) {
        case WM_INITDIALOG:
            this_.m_fInitializeSettingsDlg = true;
            this_.LoadSettings();
            CheckDlgButton(hDlg, IDC_CHECK_ENABLED, this_.m_fEnabled);
            SendDlgItemMessage(hDlg, IDC_SLIDER_BALANCE, TBM_SETRANGEMAX, FALSE, 8);
            SendDlgItemMessage(hDlg, IDC_SLIDER_BALANCE, TBM_SETPOS, TRUE, this_.m_audioBalance * 8 / 100);
            SendDlgItemMessage(hDlg, IDC_SLIDER_MAIN, TBM_SETRANGEMAX, FALSE, 8);
            SendDlgItemMessage(hDlg, IDC_SLIDER_MAIN, TBM_SETPOS, TRUE, this_.m_audioMainBalance * 8 / 100);
            SendDlgItemMessage(hDlg, IDC_SLIDER_SUB, TBM_SETRANGEMAX, FALSE, 8);
            SendDlgItemMessage(hDlg, IDC_SLIDER_SUB, TBM_SETPOS, TRUE, this_.m_audioSubBalance * 8 / 100);
            CheckDlgButton(hDlg, IDC_CHECK_MONO_MAIN, this_.m_fMonoMain);
            CheckDlgButton(hDlg, IDC_CHECK_MONO_SUB, this_.m_fMonoSub);
            SetDlgItemInt(hDlg, IDC_EDIT_DELAY, this_.m_delayMsec, FALSE);
            this_.m_fInitializeSettingsDlg = false;
            return TRUE;
        case WM_COMMAND:
            // 初期化中の無駄な再帰を省く
            if (this_.m_fInitializeSettingsDlg) {
                break;
            }
            switch (LOWORD(wParam)) {
            case IDOK:
            case IDCANCEL:
                EndDialog(hDlg, LOWORD(wParam));
                break;
            case IDC_CHECK_ENABLED:
                this_.m_pApp->EnablePlugin(!!IsDlgButtonChecked(hDlg, IDC_CHECK_ENABLED));
                break;
            case IDC_CHECK_MONO_MAIN:
            case IDC_CHECK_MONO_SUB:
                {
                    lock_recursive_mutex lock(this_.m_streamLock);
                    this_.m_fMonoMain = !!IsDlgButtonChecked(hDlg, IDC_CHECK_MONO_MAIN);
                    this_.m_fMonoSub = !!IsDlgButtonChecked(hDlg, IDC_CHECK_MONO_SUB);
                }
                WritePrivateProfileInt(TEXT("Settings"), TEXT("MonoMain"), this_.m_fMonoMain, this_.m_iniPath.c_str());
                WritePrivateProfileInt(TEXT("Settings"), TEXT("MonoSub"), this_.m_fMonoSub, this_.m_iniPath.c_str());
                break;
            case IDC_EDIT_DELAY:
                {
                    DWORD delayMsec = GetDlgItemInt(hDlg, IDC_EDIT_DELAY, nullptr, FALSE);
                    delayMsec = std::min(std::max(delayMsec, MIN_DELAY_MSEC), MAX_DELAY_MSEC);
                    if (this_.m_delayMsec != delayMsec) {
                        {
                            lock_recursive_mutex lock(this_.m_streamLock);
                            this_.m_delayMsec = delayMsec;
                            this_.ResetAudioBuffer();
                        }
                        WritePrivateProfileInt(TEXT("Settings"), TEXT("DelayMsec"), this_.m_delayMsec, this_.m_iniPath.c_str());
                    }
                }
                break;
            }
            break;
        case WM_HSCROLL:
            // 初期化中の無駄な再帰を省く
            if (this_.m_fInitializeSettingsDlg) {
                break;
            }
            switch (LOWORD(wParam)) {
            case SB_LINELEFT:
            case SB_LINERIGHT:
            case SB_PAGELEFT:
            case SB_PAGERIGHT:
            case SB_THUMBPOSITION:
                {
                    lock_recursive_mutex lock(this_.m_streamLock);
                    this_.m_audioBalance = static_cast<int>(SendDlgItemMessage(hDlg, IDC_SLIDER_BALANCE, TBM_GETPOS, 0, 0)) * 100 / 8;
                    this_.m_audioMainBalance = static_cast<int>(SendDlgItemMessage(hDlg, IDC_SLIDER_MAIN, TBM_GETPOS, 0, 0)) * 100 / 8;
                    this_.m_audioSubBalance = static_cast<int>(SendDlgItemMessage(hDlg, IDC_SLIDER_SUB, TBM_GETPOS, 0, 0)) * 100 / 8;
                }
                WritePrivateProfileInt(TEXT("Settings"), TEXT("AudioBalance"), this_.m_audioBalance, this_.m_iniPath.c_str());
                WritePrivateProfileInt(TEXT("Settings"), TEXT("AudioMainBalance"), this_.m_audioMainBalance, this_.m_iniPath.c_str());
                WritePrivateProfileInt(TEXT("Settings"), TEXT("AudioSubBalance"), this_.m_audioSubBalance, this_.m_iniPath.c_str());
                break;
            }
            break;
        }
        return FALSE;
    }

    // 設定
    tstring m_iniPath;
    bool m_fSettingsLoaded;
    bool m_fInitializeSettingsDlg;
    bool m_fEnabled;
    int m_audioBalance;
    int m_audioMainBalance;
    int m_audioSubBalance;
    bool m_fMonoMain;
    bool m_fMonoSub;
    DWORD m_delayMsec;

    std::recursive_mutex m_streamLock;
    bool m_fReset;
    int m_audioCallbackInactiveCount;

    struct DECODE_INFO
    {
        NeAACDecHandle hAACDec;
        int pid;
        std::pair<int, std::vector<BYTE>> pes;
        LONGLONG pts;
        std::vector<BYTE> workspace;
        DWORD samplerate;
        BYTE channels;
        std::vector<short> pcm;
        size_t pcmSize;
    };
    DECODE_INFO m_decodeInfo[2];
};

TVTest::CTVTestPlugin *CreatePluginClass()
{
    return new CSubAudioMixer;
}
