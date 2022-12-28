[概要]
主音声と副音声を様々な比率で合成して再生するTVTestプラグインです。

[動作環境]
・Vista以降
・TVTest ver.0.9.0 以降
・必要ランタイム: 2015-2022 Visual C++ 再頒布可能パッケージ
  ・ビルド環境: Visual Studio Express 2017 for Windows Desktop

[使い方]
SubAudioMixer.tvtpをPluginsフォルダへ。右クリック→プラグインからSubAudioMixerを
有効にすると右音声が副音声になります。プラグイン設定は 右クリックメニュー→設定
→プラグイン→SubAudioMixer から行うか、「設定画面を表示」をキー割り当てしてくだ
さい。

[注意]
いまのところ5.1ch音声は未対応です(そのまま通過します)。合成中の音量は音割れを減
らすため少し控えめです。

[ソースについて]
改変・流用など自由にしてください。
音声デコードのため静的リンクしたfaad2( https://github.com/knik0/faad2 )を使用し
ています。
