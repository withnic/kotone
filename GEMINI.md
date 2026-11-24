# AGENTS.md — 琴音（kotone）

## 1\. プロジェクト概要と参照ドキュメント

このリポジトリは、OBS Studio 用カスタム音声フィルタプラグインおよび個人声質解析ツール **「琴音（kotone）」** の実装である。設計の詳細・データモデル・DSP数理仕様・アーキテクチャについては、実装前に必ず以下のドキュメントをすべて読み込むこと。

- `docs/01_project_overview.md`: 目的・全体ワークフロー・MVPスコープ・非機能要件  
- `docs/02_data_modeling.md`: 声質プロファイルJSONスキーマ・クランプ境界値・マルチプロファイル管理・ロックフリー状態遷移・DSP数理仕様  
- `docs/03_architecture.md`: コンポーネント分割・ディレクトリ構成・リアルタイム制約・OS差異吸収・テスト戦略

>   
> **エージェント共通ファイル運用（ハードリンク構成）**: 本ファイル `AGENTS.md` を唯一のマスター定義（Single Source of Truth）とし、`CLAUDE.md` および `GEMINI.md` は本ファイルへのハードリンクとして配置する（Windows PowerShell: `New-Item -ItemType HardLink -Path CLAUDE.md -Target AGENTS.md` および `New-Item -ItemType HardLink -Path GEMINI.md -Target AGENTS.md`、Linux/WSL: `ln AGENTS.md CLAUDE.md && ln AGENTS.md GEMINI.md`）。

## 2\. 自律実行における行動規範

本プロジェクトは Codex CLI、Antigravity、Gemini CLI、および Claude Code（`claude -p` ＋ `ccw`）などの自律型コーディングエージェントを併用し、非対話モードで完全自律実装・検証を行う。エージェントは以下の行動規範を最優先で遵守すること。

- **質問・確認の禁止**: 実行中にユーザーへ質問したり、選択肢を提示して判断を仰いだりしないこと。仕様の解釈に迷う場合は、まず `docs/` 配下の設計書を確認し、それでも明記されていない細部は最も安全でシンプルかつ標準的な設計判断を自律的に採用して進めること。  
- **3段階エスカレーション修復ループ（各段階3回・計9回上限）**: ビルド・Lint・テスト失敗が発生した場合、同じ箇所の微調整を延々と繰り返すことを禁止し、以下の3段階で視点を切り替えながら自律修復を行うこと。  
  - **Level 1（試行 1〜3回目：実装デバッグ）**: 該当関数・DSP数式・型不整合の局所修正。3回で解決しなければ Level 2 へ移行する。  
  - **Level 2（試行 4〜6回目：周辺調査・構造変更）**: 中途半端なパッチをロールバック・整理した上で、テスト前提・浮動小数点許容誤差・OS差異（Windows MSVC / Linux GCC のパス・UTF-8変換・ヘッダ差異）・スレッド同期など別レイヤーを調査し、構造を変えて最大3回修正する。  
  - **Level 3（試行 7〜9回目：代替アプローチへのピボット）**: `docs/` の受け入れ基準を満たす別の技術アプローチ・シンプルな代替設計へ方針転換して組み直し、最大3回検証する。  
- 全3アプローチ（計9回）をやり切っても解決しない場合のみ試行を打ち切り、検証結果と根本原因を `BLOCKED_REPORT.md` にまとめて終了すること。  
- **テスト改ざんの禁止**: テストケースの削除、アサーションの緩和、スキップによって見かけ上グリーンにする行為は厳禁とする。

## 3\. 技術スタック・環境前提

| 項目 | 設定内容・規約 |
| :---- | :---- |
| プラグイン本体言語 | C++17（MSVC / GCC / Clang 対応、CMake 3.20+） |
| 解析CLIツール言語 | Python 3.12+（標準ライブラリ `wave`, `json`, `math`, `argparse` 中心、外部依存は `numpy` / `scipy` のみ任意利用可、未インストール環境でもフォールバック動作する構成とすること） |
| 対象OS / 環境 | Windows 11（OBS Studio 30.x/31.x ネイティブプラグイン）および Linux / WSL2（ビルド・全自動テスト実行環境） |
| 依存関係ルール | C++ DSPコアおよびJSONパーサ/シリアライザは外部ライブラリ完全ゼロ依存（ビルド時の外部ネット通信禁止） |
| 文字コード / 改行 | ソースコード・JSON・INIファイルは UTF-8（BOMなし）/ LF（`\n`）統一。Windowsファイルパスは `std::filesystem::u8path` 等で UTF-8 安全に扱うこと |

## 4\. コーディング規約・設計原則

- **リアルタイムオーディオ安全性（最重要）**: `plugin/src/dsp/` 配下のコードおよび `kotone_filter_audio` コールバック内では、動的メモリ確保（`new`, `malloc`, `std::vector` 拡張）、ファイルI/O、標準出力、ミューテックスロックを一切行わないこと。  
- **依存方向の厳守**: `plugin/src/dsp/` と `plugin/src/profile/` から `obs.h` や `windows.h` を絶対にインクルードしないこと。OBS固有コードは `plugin/src/obs/` に完全隔離する。  
- **仕様厳守（YAGNI）**: 設計書に記載のない機能（ONNX推論、独自Qt GUI等）を独断で実装しないこと。  
- **堅牢な数値計算**: フィルタ係数やゲイン計算でゼロ除算・NaN・Inf が発生しないようクランプおよび `std::isfinite` ガードを必ず入れること。

## 5\. ビルド・Lint・テスト検証コマンド

実装中および完了前には必ず `Makefile`（実体はクロスプラットフォーム対応の `scripts/check.py`）または以下のコマンドで **フォーマット・Lint・全テスト** を確認し、ツール実行漏れを防ぐこと（Claude Code 利用時は `.claude/settings.json` の `Stop` フックでも自動検証される）。

- **一括チェック（推奨・コミット/完了前必須）**: `make check`（または `python scripts/check.py check`）
  - 静的規約監査（UTF-8 BOMなし / LF 統一、`dsp/` リアルタイム安全性、`obs.h`/`windows.h` 依存隔離、日英ロケールキー完全一致）＋ `ruff` ＋ `clang-format` ＋ `clang-tidy` ＋ C++ `ctest` ＋ Python `pytest` を一括実行する。
- **自動フォーマット（C++ & Python）**: `make fmt`（`clang-format -i` + `ruff check --fix` + `ruff format`、設定は `.clang-format` と `profiler/pyproject.toml`）
- **静的解析・Lint のみ実行**: `make lint`（`clang-format --dry-run --Werror`, `clang-tidy` [`.clang-tidy`], `ruff check`, 静的規約監査）
- **C++ ビルド & 単体テスト（libobs不要モード）**: `cmake -B build -DKOTONE_BUILD_OBS_PLUGIN=OFF -DKOTONE_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build build --config Release && ctest --test-dir build --output-on-failure`
- **Python プロファイラー テスト**: `pytest profiler/tests`

※権限ガードレール（`.env` 読み書き禁止、`git push` / `git reset --hard` / `rm -rf` 等の禁止）はグローバル設定および `kekkai` 等で強制されている。