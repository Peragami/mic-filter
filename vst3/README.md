# MicFilter VST3（素の VST3 SDK 版）

フレームワーク無し。Steinberg 公式 VST3 SDK を直接使い、`../Source/dsp/PureDsp.h` の
DSP チェーンを `.vst3` プラグイン化したもの。GUI は付けない（ホストの汎用パラメーターUIに任せる）。

- プラグイン名: MicFilter / ベンダ: Takumi / カテゴリ: Fx
- Processor UID: `{190DD1F9-C10A4B07-95D6805C-70B33BBB}`
- Controller UID: `{3FD2EF38-07944434-B303F5A6-B1C4E1F1}`
- 対応: float32、モノ／ステレオ（ステレオ時は ch0 を処理して ch1 にコピー、マイク前提）
- レイテンシ報告: 0 サンプル（IIR チェーン、見越しなし）、テール: なし

パラメーター（`source/paramids.h` が定義の単一ソース）:
Input Gain / Output Gain（-24..+24 dB）、HPF On・Freq（40..200 Hz）、Gate On・Threshold（-80..-10 dB）、
EQ On・Low/Mid/High（各 -12..+12 dB）、Comp On・Threshold（-40..0 dB）・Ratio（1..10）、Limiter On、Bypass

## Windows でのビルド (VS2022)

前提: Visual Studio 2022（C++ によるデスクトップ開発）、CMake（VS 同梱で可）、Git

SDK の取得（このフォルダで初回のみ、サブモジュール込み）:

```powershell
cd mic-filter\vst3
git clone --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
```

ビルド:

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

成果物: `build\VST3\Release\MicFilter.vst3\`（フォルダ形式のバンドル）

インストールはそのフォルダを丸ごとコピー:

```powershell
copy build\VST3\Release\MicFilter.vst3 "C:\Program Files\Common Files\VST3\" /E /I
```
（要・管理者権限のターミナル）

SDK を別の場所に置く場合は `-DVST3_SDK_PATH=C:\path\to\vst3sdk` を configure 時に渡す。
`vst3sdk` フォルダが無ければ CMake が FetchContent で公式リポジトリから自動取得する。

## Linux での確認ビルド

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 成果物: build/VST3/Release/MicFilter.vst3/Contents/x86_64-linux/MicFilter.so
```

備考: VSTGUI は OFF（`SMTG_ENABLE_VSTGUI_SUPPORT=OFF` を CMake 側で強制）。
Windows の `.rc`／macOS の Info.plist は `smtg_target_configure_version_file` と
`smtg_add_vst3plugin` の標準処理に任せている（resource/ は予備）。
