# Phase 2: Python 個人声質解析CLIツール（kotone-profiler）と C++ ↔ Python 結合テスト

## 実行前提・行動規範（厳守）
- 本タスクは自律型コーディングエージェント（Codex CLI / Antigravity / Gemini CLI / `claude -p` ＋ `ccw`）により完全非対話で実行されている。ユーザーへの質問・確認・選択の提示は一切行わず、`AGENTS.md` と設計書に基づいて自律的に実装を完走させること。
- 許可されたローカルファイル操作・ビルド・テストの範囲を厳守し、リモート操作や権限外のコマンド実行は行わないこと。

## 参照ドキュメント
- `AGENTS.md`
- `docs/01_project_overview.md`
- `docs/02_data_modeling.md`
- `docs/03_architecture.md`
- Phase 1 で実装済みの `plugin/` コード

## Phase 2 実装タスク
1. **Python パッケージ骨格の作成（`profiler/`）**:
   - `profiler/pyproject.toml` および `profiler/kotone_profiler/`（`__init__.py`, `cli.py`, `wav_io.py`, `analyzer.py`, `optimizer.py`）を作成する。
   - 外部ライブラリ（`numpy` / `scipy`）が利用可能な場合は高速演算を用い、未インストール環境でも標準ライブラリ（`wave`, `struct`, `math`, `array`）のみで完全に動作するフォールバック実装を備えること。

2. **WAV読み込み・音響解析・名前付きプロファイル最適化の実装**:
   - `wav_io.py`: モノラル/ステレオ、16bit / 24bit / 32bit PCM および 32bit IEEE Float WAV の読み込み・モノラルダウンミックス。
   - `analyzer.py`:
     - 20msフレーム（10msホップ）によるノイズフロア推定（10パーセンタイル）および VAD（有声区間検出）。有声時間が `5.0秒` 未満の場合は `ERR_INSUFFICIENT_SPEECH`（終了コード `2`）を出力して終了する。
     - 自己相関法による基本周波数 $F_0$ 中央値推定、ハニング窓付き FFT による長期平均スペクトル（LTAS）、RMS分布・クレストファクター・歯擦音帯域（5kHz〜9kHz）エネルギー比の算出。
   - `optimizer.py`:
     - ターゲットプリセット（`clear_broadcast`, `warm_podcast`, `natural_flat`）およびシーン特性（囁き声のように入力RMSが低い場合も含め、目標ラウドネス `-16 LUFS` 相当へ安全に持ち上げる `input_gain_db` ＋ `makeup_gain_db` 配分）に応じた DSPパラメーターを算出し、`docs/02_data_modeling.md` 第2.1節準拠のJSON（`profile_name`, `ui_overrides` 含む）として出力する。
   - `cli.py`: `python -m kotone_profiler analyze --input <wav> --name <profile_name> --output <json> [--preset clear_broadcast] [--target-lufs -16.0]` コマンドラインインターフェースの実装。

3. **Python 単体テストおよび C++ ↔ Python 結合テスト（`profiler/tests/`）**:
   - `test_analyzer.py`: 合成テスト音声（通常トーク・低音量囁き声・過剰250Hz・6.5kHz歯擦音バースト＋無音区間）を用いた解析精度テスト、および無音/短時間WAVに対する異常系エラーテスト。
   - `test_cli_e2e.py`: CLIで「普段トーク」「ゲーム配信」「囁き声配信」等の日本語名プロファイルJSONを生成し、Phase 1でビルド済みの C++ `kotone_tests --verify-profile` に渡してエラーなく読込＆音声処理まで通る E2E 結合テスト。

## 完了条件（Exit Criteria / 3段階エスカレーション修復ループ）
- `pytest profiler/tests` および C++ 側の全テスト（`ctest --test-dir build --output-on-failure`）を実行し、すべてグリーンであることを確認すること。
- 失敗時は `AGENTS.md` 記載の **3段階エスカレーション修復ループ（Level 1〜3・計9回上限）** に従って修復し、テスト改ざんは絶対に行わないこと。
- 完了後、作成ファイル一覧・テスト結果・Phase 3 への引き継ぎ事項を出力して終了すること。
