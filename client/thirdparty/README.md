# thirdparty — 前提メモ

このフォルダに外部ライブラリは同梱しない。クライアントの依存は次の2つだけ。

## 1. Windows SDK（必須）
Win32 / WASAPI / MMDevice API。Visual Studio 2022 の
「C++ によるデスクトップ開発」ワークロードに含まれるもので足りる。
IAudioClient3（低遅延共有モード）を使うため `_WIN32_WINNT=0x0A00`
（Windows 10）以上を CMake 側で指定済み。

## 2. VST3 SDK（必須・ただし取得は別担当）
`.vst3` をホストするために Steinberg VST3 SDK の **ヘッダ** が必要。
SDK 本体は `vst3/` の作業ラインが取得・管理する前提で、ここには入れない。

- 取得元: https://github.com/steinbergmedia/vst3sdk （Steinberg VST3 ライセンス）
- 必要なのは `pluginterfaces/` と `base/` のヘッダと、インターフェース IID の
  実体定義を持つ `pluginterfaces/base/` の4ファイル
  （conststringtable.cpp / coreiids.cpp / funknown.cpp / ustring.cpp）だけ。
  これらは CMake が自動で取り込む。それ以外の SDK ソースはリンクしない
  （IHostApplication も `vst3_host.cpp` 内で素実装している）。
- 渡し方: CMake 変数 `VST3_SDK_PATH` に SDK ルート
  （`pluginterfaces/vst/ivstcomponent.h` が直下に見えるフォルダ）を指定する。
  未指定の場合は `../vst3/VST3_SDK`、`../vst3/sdk`、`../vst3/vst3sdk`、
  環境変数 `VST3_SDK_PATH` の順に探す。
- SDK が見つからない構成ではビルドは通るが、VST3 ホストはスタブになり、
  プラグイン読み込み時に「SDK 未設定」エラーを返す（client/README.md 参照）。
