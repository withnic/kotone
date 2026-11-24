# 琴音（kotone）

OBS Studio 用カスタム音声フィルタプラグインおよび個人声質解析ツール **「琴音（kotone）」**。

事前に録音したドライ音声（WAV）を `kotone-profiler` で声質解析し、配信シーン別（普段トーク / ゲーム配信 / 囁き声配信 など上限なし）のプロファイル JSON を生成。OBS Studio プラグイン本体は純粋 C++17・外部依存ゼロのリアルタイム DSP チェーン（Input Trim → Butterworth HPF → 8-Band PEQ → De-Esser → Stereo-Linked Compressor → Makeup Gain → Lookahead Brickwall Limiter）により、音切れゼロで瞬時にプロファイルを切り替え、声割れ（クリッピング）を確実に防止します。

---

## かんたん導入（GitHub Releases）

ビルド環境を用意せずに導入する場合は、[GitHub Releases](https://github.com/withnic/kotone/releases) から最新版をダウンロードしてください。

- **Windows 11**: `kotone-windows-x64.zip` を解凍し、中の `obs-plugins` と `data` フォルダを `C:\Program Files\obs-studio\`（または `%ProgramData%\obs-studio\plugins\kotone\`）へ配置して OBS Studio を再起動します。
- **Linux**: `kotone-linux-x86_64.tar.gz` をダウンロードし、`sudo tar -xzf kotone-linux-x86_64.tar.gz -C /usr` で展開して OBS Studio を再起動します。

---

## リポジトリ構成

```text
kotone/
├── CMakeLists.txt                  # C++ DSPコア・単体テスト・OBSプラグイン統合ビルド
├── CMakePresets.json               # Windows / Linux ビルドプリセット
├── AGENTS.md                       # エージェント共通規約マスター（CLAUDE.md / GEMINI.md はハードリンク）
├── docs/                           # プロジェクト設計書（01〜03）
├── plugin/
│   ├── include/kotone/             # C++17 DSPコア & マルチプロファイル管理ヘッダ
│   ├── src/dsp/                    # リアルタイム安全（ゼロアロケーション・ロックフリー）DSPエンジン
│   ├── src/profile/                # 外部依存ゼロ JSONパーサ／シリアライザ・上限なしリポジトリ
│   ├── src/obs/                    # OBS Studio プラグインアダプタ（UI・保存・ホットキー1〜10）
│   ├── data/locale/                # 日本語 (ja-JP.ini) / 英語 (en-US.ini) UIローカライズ
│   └── tests/                      # C++ 単体テストおよび `--verify-profile` CLI 検証ツール
└── profiler/
    ├── pyproject.toml              # Python 声質解析CLIパッケージ設定
    ├── kotone_profiler/            # WAV読込・VAD/F0/LTAS/歯擦音解析・DSP最適化・CLI
    └── tests/                      # Python 単体テスト & C++ 結合 E2E テスト
```

---

## 1. エージェント共通ファイル（ハードリンク設定）

本リポジトリでは `AGENTS.md` を唯一のマスター定義とし、`CLAUDE.md` および `GEMINI.md` をハードリンクとして配置します。クローン直後などでリンクが分離している場合は以下を実行してください。

### Windows PowerShell

```powershell
New-Item -ItemType HardLink -Path CLAUDE.md -Target AGENTS.md
New-Item -ItemType HardLink -Path GEMINI.md -Target AGENTS.md
```

### Linux / WSL2

```bash
ln AGENTS.md CLAUDE.md
ln AGENTS.md GEMINI.md
```

---

## 2. DSP コア & 全自動テストのビルド・実行（libobs 不要）

### C++ DSP コア & 単体テスト

```bash
cmake -B build -DKOTONE_BUILD_OBS_PLUGIN=OFF -DKOTONE_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

### Python 声質解析ツール & C++ E2E 結合テスト

```bash
pytest profiler/tests
```

### 単一プロファイル JSON の C++ 検証（CLI モード）

```bash
./build/kotone_tests --verify-profile path/to/囁き声配信.json
```

---

## 3. OBS Studio プラグインのビルド＆インストール手順

OBS Studio 30.x / 31.x の開発パッケージ（`libobs` CMake パッケージ）が導入された環境で `-DKOTONE_BUILD_OBS_PLUGIN=ON` を指定してビルドします。

### Windows 11（Visual Studio 2022 + OBS Studio SDK）

```powershell
cmake -B build-obs -G "Visual Studio 17 2022" -A x64 `
  -DKOTONE_BUILD_OBS_PLUGIN=ON `
  -DKOTONE_BUILD_TESTS=ON `
  -DCMAKE_PREFIX_PATH="C:\path\to\obs-deps;C:\path\to\obs-studio\build_x64\install"
cmake --build build-obs --config Release
cmake --install build-obs --config Release --prefix "C:\Program Files\obs-studio"
```

- プラグイン DLL 配置先: `C:\Program Files\obs-studio\obs-plugins\64bit\kotone.dll`
- ローカライズ INI 配置先: `C:\Program Files\obs-studio\data\obs-plugins\kotone\locale\ja-JP.ini` および `en-US.ini`
- デフォルトユーザープロファイル格納先: `%APPDATA%\obs-studio\plugin_config\kotone\profiles\`

### Linux（GCC / Clang + `libobs-dev`）

```bash
cmake -B build-linux-obs -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DKOTONE_BUILD_OBS_PLUGIN=ON \
  -DKOTONE_BUILD_TESTS=ON
cmake --build build-linux-obs
sudo cmake --install build-linux-obs --prefix /usr
```

- プラグイン `.so` 配置先: `/usr/lib/obs-plugins/kotone.so`（ディストリビューションにより `/usr/lib/x86_64-linux-gnu/obs-plugins/` または `~/.config/obs-studio/plugins/kotone/bin/64bit/`）
- ローカライズ INI 配置先: `/usr/share/obs/obs-plugins/kotone/locale/`
- デフォルトユーザープロファイル格納先: `~/.config/obs-studio/plugin_config/kotone/profiles/`

---

## 4. シーン別プロファイル作成＆OBS 運用ワークフロー

### Step 1: シーン別ドライ音声の録音とプロファイル生成（`kotone-profiler`）

素のマイク入力をシーン別に 1〜3 分程度 WAV（16/24/32-bit PCM または 32-bit Float）で録音し、`kotone-profiler` でプロファイル JSON を生成します（`PYTHONPATH=profiler` または `pip install -e profiler` 後）。

```bash
# 例1: 普段トーク用（クリア配信標準プリセット）
python -m kotone_profiler analyze \
  --input talk_dry.wav \
  --name "普段トーク" \
  --preset clear_broadcast \
  --output-dir "$APPDATA/obs-studio/plugin_config/kotone/profiles"

# 例2: ゲーム配信用（絶叫・ダイナミクス重視）
python -m kotone_profiler analyze \
  --input game_dry.wav \
  --name "ゲーム配信" \
  --preset clear_broadcast \
  --target-lufs -16.0 \
  --output-dir "$APPDATA/obs-studio/plugin_config/kotone/profiles"

# 例3: 囁き声・ASMR 配信用（低音量から安全に持ち上げ・温かみ重視）
python -m kotone_profiler analyze \
  --input whisper_dry.wav \
  --name "囁き声配信" \
  --preset warm_podcast \
  --target-lufs -16.0 \
  --output-dir "$APPDATA/obs-studio/plugin_config/kotone/profiles"
```

### Step 2: OBS Studio でのフィルタ適用・切替・名前付き保存

1. OBS Studio の音声ミキサーからマイクソースの **「フィルタ」→「＋」→「琴音 (Kotone) 声質フィルタ」** を追加します。
2. **「プロファイル一覧を再スキャン」** をクリックすると、内蔵プリセット 3 種に加えて、プロファイルフォルダ内のすべての `*.json`（上限なし・日本語名対応）が **「声質プロファイル選択」** ドロップダウンへ列挙されます。
3. 配信中に **「プロファイル適用度 (Mix)」**・**「出力メイクアップ補正」**・**「リミッター天井値」** をスライダーで微調整できます（Mix を 0% にしても HPF とリミッターによる声割れ保護は 100% 維持されます）。
4. 調整結果を保存したい場合は **「新規プロファイル名」** に名前を入力して **「現在の設定を新規プロファイルとして保存」** をクリック（同名がある場合は自動で `_2`, `_3` を付与して安全にアトミック保存）。選択中のユーザープロファイルを更新したい場合は **「選択中のプロファイルへ上書き保存」** を使用します（内蔵プリセット選択中は自動で新規保存にフォールバックします）。

### Step 3: ホットキー / Stream Deck でのワンタッチ切替

1. OBS Studio の **「設定」→「ホットキー」** を開き、マイクソース配下の **「琴音: ユーザープロファイル 1 へ切替」〜「ユーザープロファイル 10 へ切替」** にショートカットキーを割り当てます。
2. 配信中にキーを押すだけで、ロックフリースロット切替と 10ms ゲインスムージングにより音切れ・クリックノイズなしで即座に声質プロファイルが切り替わります。

---

## ライセンス

- 本リポジトリのソースコードは **MIT License**（[LICENSE](LICENSE)）のもとで公開されています。
- なお、OBS Studio の `libobs`（GNU General Public License v2.0 or later）とリンクしてビルド・配布される OBS プラグインバイナリ（`kotone.dll` / `kotone.so`）は、`libobs` のライセンス条件（GPL-2.0-or-later）に準拠して配布されます。
