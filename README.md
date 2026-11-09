# MicFilter — 自分用マイクフィルター (Windows)

マイクの音を「その人らしく、はっきり、近くに聴こえて、邪魔な音が何も無い」
音にするための個人用ツール。同じ DSP を 2 形態で使う。

| 成果物 | 用途 |
|---|---|
| `MicFilter.vst3` | VST3 プラグイン。DAW など他のホストでも再利用できる |
| `MicFilterClient.exe` | 専用クライアント (Win32 + WASAPI)。入出力を選んで VST をホストし、Discord 等へ流す |

構成は最軽量・低遅延を優先：C++ / フレームワークなし、DSP は自前ヘッダ、
VST3 は公式 SDK 直接、クライアントは Win32 + WASAPI 直接。

## フィルター (信号順)

入力Gain → デクリップ → デクリック → クリック音カット → ハイパス →
ハムノッチ → AIノイズ除去 (RNNoise, 任意) → ヒス除去 (スペクトル減算, 任意) →
3バンドEQ → 帯域別ノイズ抑制 → ゲート (吐息キープモードあり) → ディエッサー →
コンプレッサー → エキサイター → リミッター → 出力Gain

各フィルターはクライアントのチェックボックスで ON/OFF を即時切替できる。
既定は低遅延モード (AI なし)。AI ノイズ除去は +10ms、ヒス除去は +10.7ms。

## Discord / ゲーム等へ入れる場合

クライアントの出力は仮想オーディオケーブル (VB-CABLE 等) に向ける。

```
マイク → MicFilterClient (出力 = VB-CABLE Input) → Discord (入力 = VB-CABLE Output)
```

## ビルド

前提を 2 つ取得する (どちらもリポジトリには含めない)。

```sh
# 1) VST3 SDK (vst3/vst3sdk に置く。無ければ CMake が FetchContent で取得)
git clone --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3/vst3sdk

# 2) RNNoise のモデルデータ (公式 tarball を検証付きで取得・展開)
cd thirdparty/rnnoise && sh download_model.sh && cd ../..
```

### Windows ネイティブ (Visual Studio 2022)

```bat
cmake -S vst3 -B vst3/build
cmake --build vst3/build --config Release
cmake -S client -B client/build
cmake --build client/build --config Release
```

### Linux から MinGW クロスビルド

```sh
TC=$PWD/tools/mingw-toolchain.cmake
cmake -S vst3 -B vst3/build-mingw -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release -DSMTG_RUN_VST_VALIDATOR=OFF
cmake --build vst3/build-mingw --target MicFilter
cmake -S client -B client/build-mingw -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release
cmake --build client/build-mingw
```

## テスト (Linux)

DSP のユニットテストが `tests/` にある (合成信号での実測検証)。
RNNoise のオブジェクトを先に作ってからリンクする。手順は各テスト冒頭のコメント参照。

## ライセンス / クレジット

- RNNoise: (c) Jean-Marc Valin, BSD-3-Clause (`thirdparty/rnnoise/COPYING`)
- VST3: Steinberg VST3 ライセンスに従う
- それ以外の自前コード: 個人プロジェクト
