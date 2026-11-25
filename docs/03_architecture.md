# 03\_architecture（琴音 / kotone アーキテクチャ・ディレクトリ構成・テスト戦略 v3）

## 1\. ドキュメントの目的

本ドキュメントは、「琴音（kotone）」のコンポーネント分割、リポジトリ構成、名前付きマルチプロファイル管理（上限なし保存・一覧スキャン・ホットキー切替）、リアルタイムオーディオスレッドとUIスレッドの境界設計、OS差異吸収（Windows / Linux）、および単体・結合テスト戦略を定義する。

## 2\. システム全体アーキテクチャ

```
+-----------------------------------------------------------------------+
|  [Offline Calibration Tool: kotone-profiler (Python 3.12+)]           |
|   dry_voice.wav --(--name "囁き声配信")--> profiles/囁き声配信.json   |
+-----------------------------------------------------------------------+
                                                           |
                                   (Directory Scan / Save / Hotkey)
                                                           v
+-----------------------------------------------------------------------+
|  [OBS Studio Plugin: kotone (C++17 / libobs)]                         |
|                                                                       |
|  +-----------------------------------------------------------------+  |
|  | Presentation / OBS API Adapter (plugin/src/obs/)                |  |
|  |  - obs_source_info 登録, obs_properties_t UI構築, filter_update  |  |
|  |  - プロファイルドロップダウン動的構築 & 名前付き保存ボタン      |  |
|  |  - OBSホットキー登録 (Profile 1〜10 クイック切替)               |  |
|  +-----------------------------------------------------------------+  |
|                                  |                                    |
|  +-----------------------------------------------------------------+  |
|  | Profile Repository & Serializer (plugin/src/profile/)           |  |
|  |  - プロファイルフォルダ内の *.json 自動スキャン（上限なし）     |  |
|  |  - 純粋C++17 JSONパース・シリアライズ・アトミック保存           |  |
|  +-----------------------------------------------------------------+  |
|                                  |                                    |
|                 Lock-Free Fixed Slot Ring (Atomic Index Swap)         |
|                                  v                                    |
|  +-----------------------------------------------------------------+  |
|  | Core DSP Engine (plugin/src/dsp/)  [Pure C++17 / Zero-Alloc]    |  |
|  |  InputGain -> VoiceCleaner (Declick + Breath) -> HPF -> 8-Band   |  |
|  |  PEQ -> DeEsser -> Comp -> Makeup                                |  |
|  |  -> Lookahead Brickwall Limiter -> Hard Clamp & NaN Guard        |  |
|  +-----------------------------------------------------------------+  |
+-----------------------------------------------------------------------+
```

### 2.1 コンポーネント責務と依存方向（Clean Architecture）

1. **Core DSP Engine（`plugin/src/dsp/`）**:  
   - **完全独立・純粋C++17**: `obs.h`（libobs）や OS固有API（`windows.h` 等）に**一切依存しない**純粋なC++17ライブラリ（静的ライブラリ `kotone_dsp` としてビルド）。  
2. **Profile Repository & Serializer（`plugin/src/profile/`）**:  
   - **外部依存ゼロ・オフライン完結**: 軽量・再帰下降式の純粋C++17 JSONパーサ＆シリアライザを自前実装。  
   - **プロファイルリポジトリ機能**:  
     - `list_profiles(dir_path)`: 指定ディレクトリ内の全 `*.json` ファイルをUTF-8安全にスキャンし、登録数上限なしの `std::vector<ProfileEntry>` としてソート返却する（破損JSONはスキップし他のプロファイル一覧表示を阻害しない）。  
     - `load_profile_by_id(id, dir_path)` / `get_builtin_preset(name)`: 内蔵プリセット（3種）またはユーザー保存プロファイルを読み込み、クランプバリデーション済みの `VoiceProfile` 構造体を返す。  
     - `save_profile_new(dir_path, requested_name, profile)` / `save_profile_overwrite(file_path, profile)`: ファイル名サニタイズ、重複時の連番付与（`_2`, `_3`...）、一時ファイル経由のアトミック保存を実行する。  
3. **OBS API Adapter（`plugin/src/obs/`）**:  
   - `obs-module.h` をインクルードする唯一のレイヤー。  
   - **OBSプロパティUI設計**:  
     - `profile_select`（`OBS_COMBO_TYPE_LIST`）: 内蔵プリセット3種 ＋ `profiles/` 内の全ユーザープロファイル（上限なし、5パターンでも20パターンでも自動追加）を列挙。変更コールバック（`obs_property_set_modified_callback`）により選択瞬間にロックフリーでDSPへ反映。  
     - `refresh_profiles_btn`（`OBS_PROPERTY_BUTTON`）: フォルダ内のプロファイル一覧を再スキャンしドロップダウンを更新。  
     - `mix_percent`（スライダー `0.0〜100.0%`）、`output_makeup_trim_db`（スライダー `-12.0〜+12.0 dB`）、`limiter_ceiling_dbfs`（スライダー `-12.0〜-0.1 dBFS`）、`declick_amount_percent`（リップノイズ除去量 `0〜100%`）、`breath_amount_percent`（ブレス抑制量 `0〜100%`）、`bypass`（チェックボックス）。  
     - **プロファイル保存グループ（`OBS_GROUP_NORMAL`）**:  
       - `new_profile_name`（`OBS_TEXT_DEFAULT`）: 新規保存用の名前入力欄（例: `普段トーク`, `ゲーム配信`, `囁き声配信`）。  
       - `save_new_profile_btn`（`OBS_PROPERTY_BUTTON`）: 現在の設定＋スライダー調整値を `new_profile_name` の名前で新規JSON保存し、一覧へ追加・選択切替する。  
       - `overwrite_profile_btn`（`OBS_PROPERTY_BUTTON`）: 選択中のユーザープロファイルへ現在の微調整値を上書き保存する。  
       - `profiles_dir`（`OBS_PATH_DIRECTORY`）: プロファイル保存先フォルダ（デフォルトはOBSプラグイン設定ディレクトリ配下の `profiles/`）。  
   - **ホットキー連携（`obs_hotkey_register_source`）**:  
     - `kotone.switch_profile_1` 〜 `kotone.switch_profile_10` の10スロット分のホットキーを登録し、スキャンされたユーザープロファイルの1番目〜10番目へショートカットキー一発で切り替えられるようにする。  
4. **Offline Voice Profiler（`profiler/`）**:  
   - Python 3.12+ による個人声質解析CLIツール。`--name` オプションでJSON内の `profile_name` と出力ファイル名を指定可能。

## 3\. リポジトリ・ディレクトリ構成

```
kotone/
├── CMakeLists.txt                     # ルートCMake（DSPコア・テスト・OBSプラグイン統合ビルド）
├── CMakePresets.json                  # Windows (MSVC) / Linux (GCC/Clang) ビルドプリセット
├── CLAUDE.md                          # claude -p 自律実行用プロジェクト憲法・規約
├── README.md                          # ビルド・インストール・プロファイル作成＆切替手順
├── .gitignore
├── .claude/
│   └── settings.json                  # Stopフック（C++テスト & Python pytest 強制）
├── docs/
│   ├── 01_project_overview.md
│   ├── 02_data_modeling.md
│   └── 03_architecture.md
├── plugin/
│   ├── include/
│   │   └── kotone/
│   │       ├── dsp_types.hpp          # パラメーター構造体・定数・クランプ関数
│   │       ├── biquad.hpp             # Biquadフィルタ（HPF / PEQ、double内部精度）
│   │       ├── dynamics.hpp           # De-Esser / Stereo-Linked Comp / Lookahead Limiter
│   │       ├── voice_cleaner.hpp      # リップノイズ除去 + ブレス抑制（固定2ms先読み・検出定数）
│   │       ├── dsp_engine.hpp         # マルチチャンネル統合DSPパイプライン & スロット管理
│   │       └── profile_loader.hpp     # 純粋C++17 JSONパーサ・シリアライザ・マルチプロファイル管理
│   ├── src/
│   │   ├── dsp/
│   │   │   ├── dsp_types.cpp
│   │   │   ├── biquad.cpp
│   │   │   ├── dynamics.cpp
│   │   │   ├── voice_cleaner.cpp
│   │   │   └── dsp_engine.cpp
│   │   ├── profile/
│   │   │   └── profile_loader.cpp
│   │   └── obs/
│   │       ├── plugin_main.cpp        # OBS_DECLARE_MODULE() エントリーポイント
│   │       └── kotone_filter.cpp      # obs_source_info 実装・UI・名前付き保存・ホットキー同期
│   ├── data/
│   │   └── locale/
│   │       ├── en-US.ini              # 英語UIラベル
│   │       └── ja-JP.ini              # 日本語UIラベル（琴音 UI表示用）
│   └── tests/
│       ├── test_main.cpp              # ゼロ依存・決定論的C++単体テストランナー（CLIプロファイル検証モード付）
│       ├── test_dsp_limiter.cpp       # 声割れ防止（天井値保証）・過大入力・NaNガード検証
│       ├── test_dsp_eq_comp.cpp       # HPF/PEQ周波数応答・ステレオリンクコンプ圧縮特性検証
│       ├── test_profile_loader.cpp    # JSONパース・名前付き保存/上書き・一覧スキャン・日本語パス検証
│       └── test_voice_cleaner.cpp     # リップノイズ除去・ブレス抑制・透過性・JSON互換検証
└── profiler/
    ├── pyproject.toml                 # Pythonプロジェクト定義（pytest設定含む）
    ├── README.md                      # 配布 ZIP 同梱の利用者向け手順
    ├── analyze_voice.bat              # Windows ドラッグ＆ドロップ起動（wizard、同梱の埋め込み Python を優先）
    ├── kotone_profiler/
    │   ├── __init__.py
    │   ├── cli.py                     # argparse CLIエントリーポイント（--name 対応）
    │   ├── wav_io.py                  # WAV読込（16/24/32bit PCM & Float対応）
    │   ├── analyzer.py                # VAD・F0推定・LTAS・ダイナミクス・歯擦音解析
│   ├── cleaner.py                 # リップノイズ・ブレス検出（C++と同一）・除外・しきい値学習
    │   └── optimizer.py               # DSPパラメーター最適化＆JSONスキーマ出力
    └── tests/
        ├── test_analyzer.py           # 合成音声によるVAD・F0・スペクトル解析テスト
        ├── test_cleaner.py            # リップノイズ・ブレス検出・解析除外・学習しきい値テスト
        └── test_cli_e2e.py            # WAV入力 -> JSON生成 -> C++バイナリ検証E2Eテスト
```

## 4\. リアルタイム安全性・OS差異吸収

### 4.1 オーディオスレッドのリアルタイム制約（Strict Real-Time Safety）

- **ヒープ確保ゼロ**: `DspEngine::process()` 内では `new` / `delete` / `malloc` / `std::string` / `std::vector` リサイズを一切行わない。最大チャンネル数（8ch）および最大サンプルレート（192kHz）に対応した固定長リングバッファを `DspEngine` コンストラクタ内で事前確保する。  
- **ロックフリー固定スロットリング（Use-After-Free 構造的排除）**:  
  - `DspEngine` 内部に固定長 `16` 要素の `SnapshotSlot` 配列（各スロットが `DspConfigSnapshot` 実体と `std::atomic<uint32_t> readers{0}` を保持）を事前確保する。  
  - ユーザーがUIドロップダウンやホットキーでプロファイルを切り替えた際、UIスレッド側でファイル読込・係数計算を行い、空きスロットへ書き込んで `active_slot_.store(slot_idx, std::memory_order_release)` で切り替える。オーディオスレッドは一切ディスクI/Oやロックを行わない。  
- **パラメータスムージング（ジッパーノイズ防止）**:  
  - プロファイル切替時（「普段トーク」→「囁き声配信」などゲイン差が大きい切替時）の急激な音量ジャンプによる「プチッ」というクリックノイズを防ぐため、入力ゲイン・メイクアップゲインには1次IIRスムーザー（時定数 `10ms`）を適用する。

### 4.2 OS / ビルド環境差異の吸収

- **OBS SDK なしでのビルド・テスト両立**:  
  - CMake オプション `-DKOTONE_BUILD_OBS_PLUGIN=ON/OFF` を設ける。`OFF`（デフォルト）で `kotone_dsp` と `kotone_tests` が完全にコンパイル・実行できる。  
- **Windows / Linux UTF-8 ファイルパスとディレクトリ走査**:  
  - 日本語プロファイル名（例: `普段トーク.json`, `ゲーム配信.json`, `囁き声配信.json`）を安全に扱うため、ディレクトリ走査・ファイル作成・読み書きには必ず `std::filesystem::u8path`（C++17）を使用し、CP932（Shift\_JIS）文字化けによるファイルオープン失敗を防ぐ。

## 5\. テスト戦略・受け入れ基準（Acceptance Criteria）

### 5.1 C++ DSP & マルチプロファイルテスト（`ctest` / `kotone_tests`）

1. **声割れ防止（ブリックウォール保証）テスト**:  
   - `+12 dBFS`（振幅 `3.98f`）の突発サイン波・パルス波を `DspEngine` に入力し、出力全サンプルの絶対値が `ceiling_dbfs`（`-1.0 dBFS` \= `0.89125f`）以下であることを検証する（許容誤差 `1e-5f` 以内）。  
2. **HPF / 8-Band PEQ 周波数特性テスト**:  
   - `40Hz`（HPFカットオフ `85Hz` 以下）のサイン波が減衰（`-12dB`以上）し、`1000Hz` の通過帯域サイン波が `±0.2dB` 以内で通過することを検証する。  
3. **コンプレッサー圧縮特性・ステレオリンクテスト**:  
   - Threshold `-20dBFS`, Ratio `4.0` の設定で `-8dBFS` の定常サイン波を入力し、定常状態の出力レベルが理論値（`-17dBFS ± 0.5dB`）に収束することを検証する。また、Lchのみ大音量・Rch無音入力時に両チャンネルへ同一ゲインリダクションが適用されることを確認する。  
4. **マルチプロファイル管理（上限なし保存・一覧スキャン・日本語名・重複回避）テスト**:  
   - 一時ディレクトリに対して「普段トーク」「ゲーム配信」「囁き声配信」「深夜雑談」「歌枠用」「検証用6」の6パターン以上を `save_profile_new()` で連続保存し、すべて欠落なく保存・`list_profiles()` で列挙・再読み込みできることを検証する。  
   - 同名保存時の自動連番サフィックス（`_2`）付与、禁止文字（`ゲーム/配信:用`）の安全サニタイズ、既存プロファイルの上書き保存（`save_profile_overwrite()`）、および破損JSON混在時のスキップ耐性を検証する。  
5. **異常入力・スレッド安全性テスト**:  
   - `NaN` / `Inf` 入力からの `0.0f` 復旧、境界値クランプ、および別スレッドからの1000回連続プロファイル切替＋並行 `process()` 実行で競合・クラッシュが起きないことを検証する。  
6. **Voice Cleaner（リップノイズ除去・ブレス抑制）テスト**:
   - 無効時・有効時とも、定常の有声音が固定 `2.0ms`（48kHz で 96 サンプル）遅延のみで透過すること（誤差 `1e-6` 以下、クリック検出 0 件）。
   - 孤立したクリックをすべて検出し、高域エネルギーを `8dB` 以上減衰させ、クリック以外の区間は無加工で通すこと。滑らかな発声の立ち上がり・定常ノイズではクリックを誤検出しないこと。
   - 高域寄りのブレス状ノイズ（-48dBFS）が `7〜9.5dB` 抑制され、前後の有声区間は `±0.2dB` 以内で保たれること。`input_gain_db` に追従するしきい値、UI の抑制量スライダー、schema `1.0.0` 互換（無効扱い）、往復シリアライズ、クランプ、8kHz〜192kHz での有限出力を検証する。
7. **CLIプロファイル検証モード（`kotone_tests --verify-profile <path>`）**:  
   - Python側E2Eテストから呼び出せる検証モードを実装する。

### 5.2 Python プロファイラーテスト（`pytest`）

1. **合成音声による解析精度テスト**:  
   - 既知の基本周波数（`130Hz`）、ノイズフロア（`-60dBFS`）、過剰な低中音（`250Hz` ブースト）と歯擦音（`6.5kHz` バースト）を持つ合成WAVから適切なEQ/HPF/De-Esserパラメーターを算出できることを検証する。  
2. **名前付きプロファイル出力 E2E テスト**:  
   - `kotone-profiler analyze --input ... --name "囁き声配信" --output ...` で生成した日本語名プロファイルJSONを `kotone_tests --verify-profile` へ渡し、C++側で正常ロード・DSP処理完了することを検証する。