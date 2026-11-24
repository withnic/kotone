# Phase 1: C++ コアDSPエンジン・マルチプロファイルリポジトリ・単体テスト基盤の実装

## 実行前提・行動規範（厳守）
- 本タスクは自律型コーディングエージェント（Codex CLI / Antigravity / Gemini CLI / `claude -p` ＋ `ccw`）により完全非対話で実行されている。ユーザーへの質問・確認・選択肢の提示は一切行わず、`AGENTS.md`（ハードリンク先の `CLAUDE.md` / `GEMINI.md` 含む）および `docs/` 配下の設計書に従って自律的に実装を完了させること。
- ローカルファイル操作・ビルド・テストコマンドの範囲内に厳密にとどめ、`git push` や許可外のシステム変更は絶対に行わないこと。

## 参照ドキュメント
実装前に必ず以下のファイルをすべて読み込み、仕様を完全に把握すること。
- `AGENTS.md`
- `docs/01_project_overview.md`
- `docs/02_data_modeling.md`
- `docs/03_architecture.md`

## Phase 1 実装タスク
1. **リポジトリ骨格・CMakeビルド構成の作成**:
   - `docs/03_architecture.md` のディレクトリ構成に従い、`CMakeLists.txt`、`CMakePresets.json`、`.gitignore`、`plugin/` 配下のヘッダ・ソース雛形を作成する。
   - リポジトリ直下に `AGENTS.md` を配置し、`CLAUDE.md` と `GEMINI.md` を `AGENTS.md` へのハードリンクとして作成（または未作成時の自動リンク手順を `README.md` に明記）すること。
   - `CMakeLists.txt` は `-DKOTONE_BUILD_OBS_PLUGIN=OFF`（デフォルト）で `libobs` なしに `kotone_dsp` 静的ライブラリと `kotone_tests` 実行ファイルをビルドできるように構成する。
   - 外部ネット通信なしで即コンパイルできるよう、外部ライブラリゼロ依存の純粋C++17 JSONパーサ＆シリアライザを `plugin/src/profile/` に実装する。

2. **DSPコアアルゴリズムの実装（`plugin/include/kotone/`, `plugin/src/dsp/`）**:
   - `dsp_types.hpp / .cpp`: `VoiceProfile`、`DspConfigSnapshot`、全パラメーター構造体、および `docs/02_data_modeling.md` 第2.2節のクランプ境界値関数の実装。
   - `biquad.hpp / .cpp`: Direct Form II Transposed による Biquad フィルタ（2次/4次 Butterworth HPF、Peaking / Low-Shelf / High-Shelf EQ）。係数および状態変数（`z1, z2`）は `double` 精度＋Flush-to-Zero（Denormal防止）で実装すること。
   - `dynamics.hpp / .cpp`:
     - De-Esser（4kHz〜10kHz バンドパスサイドチェーン検波によるダイナミック減衰）。
     - Stereo-Linked RMS/Peak ハイブリッド・コンプレッサー（Soft-knee、全有効チャンネル共通ゲインリダクション）。
     - ルックアヘッド・ブリックウォールリミッター（事前確保済み固定長リングバッファ、先読みピーク検出、最終段ハードクランプ `[-ceiling_linear, +ceiling_linear]`、NaN/Inf 検出時の安全ミュート＆状態リセット）。
   - `dsp_engine.hpp / .cpp`:
     - 入力ゲイン・メイクアップゲインスムージング（時定数 `10ms`、プロファイル切替時のクリックノイズ防止）→ HPF → 8-Band PEQ → De-Esser → Compressor → Makeup Gain → Lookahead Limiter の最大8チャンネル処理パイプライン。
     - 16スロットの固定長 `SnapshotSlot` リング配列（`std::atomic<uint32_t> readers` 参照カウント付き）による完全ロックフリーなプロファイル切替。動的メモリ確保・ロックは一切使用しないこと。

3. **マルチプロファイルリポジトリ・名前付き保存/読込の実装（`plugin/src/profile/`）**:
   - UTF-8ファイルパス（`std::filesystem::u8path`）対応のJSON読込・書き出し・バリデーション・クランプ処理。
   - 内蔵プリセット（`builtin:neutral_safe`, `builtin:clear_broadcast`, `builtin:warm_podcast`）および `mix_percent`（0〜100%）補間による `DspConfigSnapshot` 生成ロジック。
   - 上限なしのマルチプロファイル管理機能：
     - `list_profiles(dir_path)`: ディレクトリ内の全 `*.json` をスキャンして名前付きプロファイル一覧を返却（上限なし、破損JSONはスキップ）。
     - `save_profile_new(dir_path, requested_name, profile)`: ファイル名サニタイズ、日本語名対応、重複時の連番サフィックス（`_2`, `_3`...）自動付与、一時ファイル（`.tmp`）経由のアトミック保存。
     - `save_profile_overwrite(file_path, profile)`: 既存ユーザープロファイルのアトミック上書き保存。

4. **C++ 単体テストの作成（`plugin/tests/`）**:
   - `docs/03_architecture.md` 第5.1節の全受け入れ基準（声割れ防止の天井値保証テスト、HPF/PEQ周波数応答テスト、ステレオリンクコンプレッサー圧縮特性テスト、6パターン以上の名前付きプロファイル連続保存・一覧スキャン・重複連番・禁止文字サニタイズ・上書き保存テスト、NaN/Infリカバリテスト、マルチスレッド並行スナップショット切替テスト、および `--verify-profile <path>` CLI検証モード）をすべて実装する。

## 完了条件（Exit Criteria / 3段階エスカレーション修復ループ）
- 以下のコマンドを実行し、コンパイル警告・エラーゼロ、全テストパスを確認すること。
  `cmake -B build -DKOTONE_BUILD_OBS_PLUGIN=OFF -DKOTONE_BUILD_TESTS=ON && cmake --build build --config Release && ctest --test-dir build --output-on-failure`
- エラーやテスト失敗が発生した場合は、同一箇所の無限ループを防ぐため、以下の**3段階エスカレーション修復ループ（各段階3回・計9回上限）**に従って解決すること。
  1. **Level 1（試行 1〜3回目：実装レベルのデバッグ）**: エラー出力から該当関数・DSP数式・型不整合を特定して修正する。3回で解決しない場合は同じ関数の小手先の修正を禁止し、Level 2 へ移行する。
  2. **Level 2（試行 4〜6回目：周辺環境・テスト前提の調査と構造変更）**: 中途半端なパッチをロールバック・整理した上で、テスト信号のサンプリング条件・過渡応答時間・浮動小数点精度・OS差異（UTF-8パス等）を調査し、構造を変えて最大3回修正する。
  3. **Level 3（試行 7〜9回目：代替アプローチへのピボット）**: `docs/` の設計書に立ち返り、受け入れ基準を満たす別の技術アプローチやシンプルな代替設計へ方針転換して組み直し、最大3回検証する。
- **品質・コンテキスト維持ルール**:
  - **テストの改ざん禁止**: アサーションの削除・緩和やテストのスキップによって見かけ上パスさせる行為は厳禁とする。
  - **ログ出力の絞り込み**: テスト失敗時は全ログを垂れ流さず、失敗したテストケースとエラー出力に絞って確認すること。
  - **最終セーフティネット（計9回完了後も未解決の場合）**: 試した3つのアプローチと根本原因を `BLOCKED_REPORT.md` にまとめて終了すること。
- 全検証がグリーンになったことを確認した上で、作成したファイル一覧・テスト結果・Phase 2 への引き継ぎ事項を簡潔に出力して終了すること。
