# Phase 3: OBS Studio プラグインアダプタ・名前付きマルチプロファイルUI・ホットキー・ドキュメント整備

## 実行前提・行動規範（厳守）
- 本タスクは自律型コーディングエージェント（Codex CLI / Antigravity / Gemini CLI / `claude -p` ＋ `ccw`）により完全非対話で実行されている。ユーザーへの質問・確認は一切行わず、`AGENTS.md` と設計書に基づいて自律的に実装を完走させること。
- 許可された操作範囲を厳守すること。

## 参照ドキュメント
- `AGENTS.md`
- `docs/01_project_overview.md`
- `docs/02_data_modeling.md`
- `docs/03_architecture.md`
- Phase 1・Phase 2 の実装コード一式

## Phase 3 実装タスク
1. **OBS Studio プラグインアダプタの実装（`plugin/src/obs/`）**:
   - `plugin_main.cpp`: `OBS_DECLARE_MODULE()`, `OBS_MODULE_USE_DEFAULT_LOCALE("kotone", "en-US")`, `obs_module_load()`, `obs_module_unload()` の実装。
   - `kotone_filter.cpp`:
     - `obs_source_info` 構造体（`id = "kotone_voice_filter"`, `type = OBS_SOURCE_TYPE_FILTER`, `output_flags = OBS_SOURCE_AUDIO`）の登録。
     - `get_properties`:
       - **プロファイル選択ドロップダウン（`profile_select`）**: 内蔵プリセット3種 ＋ `profiles/` フォルダ内の全 `*.json` ユーザープロファイル（上限なし、5パターン以上自由に登録・選択可）を動的列挙。
       - **一覧再スキャンボタン（`refresh_profiles_btn`）**: 外部ツール（`kotone-profiler`）で追加したJSONを即座にドロップダウンへ反映。
       - **リアルタイム微調整スライダー**: プロファイル適用度（`mix_percent`: `0〜100%`）、出力メイクアップ補正（`output_makeup_trim_db`: `-12.0〜+12.0 dB`）、リミッター天井（`limiter_ceiling_dbfs`: `-12.0〜-0.1 dBFS`）、バイパス（`bypass`）。
       - **名前付きプロファイル保存UI**: 新規プロファイル名入力ボックス（`new_profile_name`）、「新規プロファイルとして保存」ボタン（`save_new_profile_btn`）、選択中プロファイルへの「上書き保存」ボタン（`overwrite_profile_btn`）、プロファイル保存先フォルダ指定（`profiles_dir`）。
     - **OBSホットキー登録（`obs_hotkey_register_source`）**:
       - 登録済みユーザープロファイル（1番目〜10番目）をショートカットキーや外部デバイスからワンタッチで切り替えられるホットキーを登録・復元（`save` / `load` コールバック対応）。
     - `filter_audio`: `obs_audio_data` の各チャンネルポインタ（`data[0..7]`）に対して `DspEngine::process()` をゼロアロケーションで呼び出し、タイムスタンプとフレーム数を維持して返却。

2. **UIローカライズリソースの整備（`plugin/data/locale/`）**:
   - `ja-JP.ini` および `en-US.ini` を作成し、プロファイル選択・新規名前付き保存・上書き保存・ホットキー名称を含む全UI項目が自然な日本語・英語で表示されるようにする。

3. **CMake OBSビルド統合・README整備**:
   - `CMakeLists.txt` の `-DKOTONE_BUILD_OBS_PLUGIN=ON` パスを完成させ、`libobs` リンク設定および `data/locale/` のインストールターゲットを定義する。
   - `README.md` に、OBSプラグインのビルド手順、インストール先パス、`AGENTS.md` / `CLAUDE.md` / `GEMINI.md` ハードリンク作成コマンド、および `kotone-profiler` を使ったシーン別（普段トーク / ゲーム配信 / 囁き声配信 等）プロファイル作成＆OBS上での保存・ホットキー切替手順を記載する。

## 完了条件（Exit Criteria / 3段階エスカレーション修復ループ）
- C++ テスト（`cmake -B build -DKOTONE_BUILD_OBS_PLUGIN=OFF -DKOTONE_BUILD_TESTS=ON && cmake --build build --config Release && ctest --test-dir build --output-on-failure`）および Python テスト（`pytest profiler/tests`）がすべてグリーンであることを確認すること。
- 失敗時は `AGENTS.md` 記載の **3段階エスカレーション修復ループ（Level 1〜3・計9回上限）** に従って修復すること。
- 最終的なリポジトリ構成、実装済み機能サマリー、および実際のOBS環境でのビルド・導入手順を出力して完了とすること。
