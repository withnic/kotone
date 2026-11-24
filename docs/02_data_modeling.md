# 02\_data\_modeling（琴音 / kotone データモデル・入出力スキーマ・DSP数理仕様 v3）

## 1\. ドキュメントの目的

本ドキュメントは、「琴音（kotone）」における名前付き声質プロファイルJSONスキーマ、マルチプロファイル管理モデル（上限なし保存・読込・切替）、OBSプラグイン内部のDSPパラメーター構造体、状態遷移（スレッド間ロックフリー同期）、およびエラー仕様を厳密に定義する。

## 2\. 声質プロファイル JSON スキーマ（`*.json`）

Python CLI（`kotone-profiler`）が出力し、C++ OBSプラグインが読み書きする設定ファイル仕様。文字コードは UTF-8（BOMなし）、改行は LF。ファイルサイズ上限は `1MB`（超過時は読込拒否）とする。

### 2.1 JSON 構造定義

```json
{
  "schema_version": "1.0.0",
  "profile_name": "囁き声配信（ASMR・深夜）",
  "created_at": "2026-11-24T18:30:00Z",
  "updated_at": "2026-11-24T18:35:00Z",
  "target_preset": "warm_podcast",
  "ui_overrides": {
    "mix_percent": 100.0,
    "output_makeup_trim_db": 0.0,
    "limiter_ceiling_dbfs": -1.0
  },
  "analysis_metrics": {
    "sample_rate_hz": 48000,
    "voiced_duration_sec": 64.2,
    "noise_floor_dbfs": -62.1,
    "mean_rms_dbfs": -34.8,
    "peak_dbfs": -14.5,
    "crest_factor_db": 20.3,
    "fundamental_freq_median_hz": 115.0,
    "sibilance_ratio": 0.21
  },
  "dsp_params": {
    "input_gain_db": 6.0,
    "hpf": {
      "enabled": true,
      "cutoff_hz": 75.0,
      "slope_db_per_oct": 24
    },
    "eq_bands": [
      { "index": 0, "enabled": true, "type": "low_shelf", "freq_hz": 120.0, "gain_db": 1.5, "q": 0.707 },
      { "index": 1, "enabled": true, "type": "peaking",   "freq_hz": 250.0, "gain_db": -2.5, "q": 2.0 },
      { "index": 2, "enabled": true, "type": "peaking",   "freq_hz": 500.0, "gain_db": -1.2, "q": 1.8 },
      { "index": 3, "enabled": true, "type": "peaking",   "freq_hz": 1500.0, "gain_db": 1.0, "q": 1.4 },
      { "index": 4, "enabled": true, "type": "peaking",   "freq_hz": 3200.0, "gain_db": 2.2, "q": 1.6 },
      { "index": 5, "enabled": true, "type": "peaking",   "freq_hz": 5000.0, "gain_db": 0.8, "q": 2.0 },
      { "index": 6, "enabled": true, "type": "peaking",   "freq_hz": 7200.0, "gain_db": -3.5, "q": 3.8 },
      { "index": 7, "enabled": true, "type": "high_shelf","freq_hz": 11000.0,"gain_db": 2.4, "q": 0.707 }
    ],
    "deesser": {
      "enabled": true,
      "freq_hz": 6800.0,
      "bandwidth_q": 2.8,
      "threshold_dbfs": -28.0,
      "max_reduction_db": 8.0,
      "attack_ms": 1.0,
      "release_ms": 40.0
    },
    "compressor": {
      "enabled": true,
      "threshold_dbfs": -28.0,
      "ratio": 4.2,
      "knee_db": 8.0,
      "attack_ms": 6.0,
      "release_ms": 110.0,
      "makeup_gain_db": 11.5
    },
    "limiter": {
      "enabled": true,
      "ceiling_dbfs": -1.0,
      "lookahead_ms": 2.0,
      "release_ms": 50.0
    }
  }
}
```

※ `ui_overrides` および `updated_at` はオプショナルフィールドとし、未記載の旧JSONや最小JSONを読み込んだ場合もデフォルト値（`mix_percent: 100.0`, `output_makeup_trim_db: 0.0`, `limiter_ceiling_dbfs: dsp_params.limiter.ceiling_dbfs`）で互換動作させる。

### 2.2 バリデーション・クランプ境界値仕様（C++ / Python 共通）

不正な値や極端な数値による発振・爆音・ゼロ除算を防ぐため、読み込み時に以下の許容範囲（Clamp Range）を厳密に適用する。数値フィールドが `NaN` / `Inf` または非数値型の場合はパースエラーとする。

| フィールド | 型 | 最小値 | 最大値 | デフォルト値 | 異常時挙動 |
| :---- | :---- | :---- | :---- | :---- | :---- |
| `schema_version` | string | `"1.0.0"` | `"1.x.x"` | `"1.0.0"` | メジャーバージョン不一致（`2.0.0`等）は読込拒否（`ERR_SCHEMA_UNSUPPORTED`） |
| `profile_name` | string | 1文字 | 64文字 | ファイル名（拡張子除） | 空文字・欠落時はファイル名を使用、64文字超過時はUTF-8文字境界で切り詰め |
| `ui_overrides.mix_percent` | float | `0.0` | `100.0` | `100.0` | クランプ |
| `ui_overrides.output_makeup_trim_db` | float | `-12.0` | `+12.0` | `0.0` | クランプ |
| `input_gain_db` | float | `-24.0` | `+24.0` | `0.0` | クランプ |
| `hpf.cutoff_hz` | float | `20.0` | `300.0` | `80.0` | サンプルレート Nyquist (`fs * 0.45`) 未満にも同時クランプ |
| `hpf.slope_db_per_oct` | int | `12` | `24` | `24` | `12` または `24` 以外は `24` にフォールバック |
| `eq_bands` 要素数 | array | `1` | `8` | `8` | 8バンド未満は残りを `enabled: false` で補完、9以上は先頭8件採用 |
| `eq_bands[i].type` | string | \- | \- | `"peaking"` | `"peaking"`, `"low_shelf"`, `"high_shelf"` 以外はパースエラー |
| `eq_bands[i].freq_hz` | float | `20.0` | `20000.0` | `1000.0` | サンプルレート Nyquist (`fs * 0.48`) 未満に同時クランプ |
| `eq_bands[i].gain_db` | float | `-18.0` | `+18.0` | `0.0` | クランプ |
| `eq_bands[i].q` | float | `0.1` | `18.0` | `1.0` | クランプ（0以下によるゼロ除算防止） |
| `deesser.freq_hz` | float | `3000.0` | `12000.0` | `6500.0` | クランプ |
| `deesser.threshold_dbfs` | float | `-48.0` | `0.0` | `-22.0` | クランプ |
| `deesser.max_reduction_db` | float | `0.0` | `18.0` | `6.0` | クランプ |
| `compressor.threshold_dbfs` | float | `-60.0` | `0.0` | `-20.0` | クランプ |
| `compressor.ratio` | float | `1.0` | `20.0` | `3.0` | クランプ（`1.0`未満防止） |
| `compressor.knee_db` | float | `0.0` | `24.0` | `6.0` | クランプ |
| `compressor.attack_ms` | float | `0.1` | `200.0` | `10.0` | クランプ |
| `compressor.release_ms` | float | `10.0` | `1000.0` | `100.0` | クランプ |
| `compressor.makeup_gain_db` | float | `-12.0` | `+24.0` | `4.0` | クランプ |
| `limiter.ceiling_dbfs` | float | `-12.0` | `-0.1` | `-1.0` | クランプ（`0.0dBFS` 以上のクリップ防止のため上限 `-0.1`） |
| `limiter.lookahead_ms` | float | `0.0` | `5.0` | `2.0` | クランプ（事前確保リングバッファ最大5msに収める） |
| `limiter.release_ms` | float | `5.0` | `500.0` | `50.0` | クランプ |

## 3\. マルチプロファイル管理モデル（上限なし保存・切替仕様）

### 3.1 プロファイルディレクトリ構造と識別子

- **デフォルト格納フォルダ**:  
  - OBSプラグインのモジュール設定パス `obs_module_config_path("profiles")`（Windows例: `%APPDATA%\obs-studio\plugin_config\kotone\profiles\`、Linux例: `~/.config/obs-studio/plugin_config/kotone/profiles/`）。  
  - 初回起動時にフォルダが存在しない場合は自動作成（`std::filesystem::create_directories`）し、サンプルの初期ユーザープロファイル雛形（例: `01_普段トーク.json`, `02_ゲーム配信.json`, `03_囁き声配信.json` の3テンプレート、または内蔵プリセット）を配置可能とする（既存ファイルがある場合は上書きしない）。  
- **ドロップダウン項目識別子（`selected_profile_id`）**:  
  - 内蔵プリセット（読み取り専用）:  
    - `"builtin:neutral_safe"`（内蔵: ニュートラル安全保護）  
    - `"builtin:clear_broadcast"`（内蔵: クリア配信標準）  
    - `"builtin:warm_podcast"`（内蔵: 温かみ・低音重視）  
  - ユーザー保存プロファイル（上限なし・動的スキャン）:  
    - `"file:<ファイル名>"`（例: `"file:普段トーク.json"`, `"file:ゲーム配信.json"`, `"file:囁き声配信.json"`, `"file:深夜雑談.json"`, `"file:歌枠用.json"` ... 制限なし）。  
    - ドロップダウンの表示ラベルには JSON内の `profile_name`（未設定時は拡張子を除いたファイル名）を表示し、重複名がある場合は `profile_name (ファイル名)` として一意に区別する。

### 3.2 名前付き保存（Save / Save As New Profile）仕様

1. **新規名前付き保存（Save As New Profile）**:  
   - UIのテキスト入力欄 `new_profile_name` に任意の名前（例: `"ゲーム配信（絶叫対策）"`）を入力し、「新規プロファイルとして保存」ボタンをクリックする。  
   - ファイル名サニタイズ規則:  
     - 制御文字（`0x00`〜`0x1F`）および Windows/POSIX 禁止文字（`\ / : * ? " < > |`）を `_` に置換し、前後の空白・ドットをトリムする。空になった場合は `"custom_profile"` とする。  
     - UTF-8 日本語文字（ひらがな・カタカナ・漢字・英数字・ハイフン・アンダースコア・括弧）はそのまま保持する（例: `ゲーム配信_絶叫対策.json`）。  
     - 同名ファイルが既に存在する場合はエラーにせず自動で連番サフィックス（`_2`, `_3` ...）を付与して衝突を回避し、新規追加する。  
   - アトミック書き込み: `<target>.json.tmp` に UTF-8 で書き出し・フラッシュした後、`std::filesystem::rename` で `<target>.json` へ置き換える。  
   - 保存完了後、プロファイル一覧を即座に再スキャンし、選択中プロファイル（`selected_profile_id`）を今保存した新規プロファイルへ自動切替する。  
2. **上書き保存（Overwrite Current Profile）**:  
   - 現在 `"file:..."` 系のユーザープロファイルを選択中の場合、UIスライダーで微調整した `ui_overrides`（`mix_percent`, `output_makeup_trim_db`, `limiter_ceiling_dbfs`）および実行DSPパラメーターを現在のJSONファイルへアトミック上書き保存する。  
   - `"builtin:..."` 選択中に押された場合は、自動的に「新規名前付き保存」と同じ新規ファイル作成フローへフォールバックし、内蔵プリセット破壊を防ぐ。

### 3.3 スレッドモデルとロックフリー更新（状態遷移）

- **状態一覧**:  
  - `UNINITIALIZED`: フィルタ生成直後。内蔵ニュートラルプロファイル（フラットEQ \+ 安全リミッター `-1.0 dBFS`）を初期ロード。  
  - `ACTIVE_PROFILE`: 有効な内蔵プリセットまたはユーザーJSONプロファイルを適用中。  
  - `FALLBACK_SAFE`: 指定JSONが削除・移動された、または破損・パースエラーの場合。直前の有効プロファイルを維持するか、初回失敗時は内蔵安全プロファイルへフォールバックし、音声を途切れさせない。  
  - `BYPASSED`: ユーザーによるバイパス有効時。入力バッファを無加工で透過（再有効化時はディレイラインとエンベロープ状態をリセットしてクリックノイズを防止）。  
- **ロックフリー固定スロットリング（Atomic Slot Index Swap ＋ 参照カウント解放）**:  
  - `DspEngine` 内部に固定長 `16` 要素の `SnapshotSlot` 配列（各スロットが `DspConfigSnapshot` 実体と `std::atomic<uint32_t> readers{0}` を保持）を事前確保する。  
  - UIスレッドは空きスロットへ新しい `DspConfigSnapshot` を書き込み、`active_slot_.store(new_idx, std::memory_order_release)` で切り替える。オーディオスレッドは `readers` をインクリメント/デクリメントして待機ゼロ・ヒープ解放ゼロで安全に読み出す。  
- **Mix（プロファイル適用度 `0.0`〜`1.0`）の動的補間**:  
  - `mix = mix_percent / 100.0` とし、EQの `gain_db` に `mix` を乗算、コンプレッサーの `ratio` を `1.0 + (ratio - 1.0) * mix`、`makeup_gain_db` に `mix` を乗算する。HPF とリミッター（声割れ防止の天井制限）は安全保護のため `mix` に関わらず100%有効を維持する。

## 4\. DSPアルゴリズム数理仕様（Audio EQ Cookbook / 標準DSP準拠）

- **サンプリングレート**: `8000Hz`〜`192000Hz`（範囲外は `48000Hz` にフォールバック）。  
- **チャンネル数**: 最大8チャンネル（`MAX_AV_PLANES = 8`）分のフィルタ状態を事前確保。  
- **Biquad フィルタ（HPF / 8-Band PEQ）**:  
  - Direct Form II Transposed 実装。係数および状態変数（`z1, z2`）は **`double`（64bit浮動小数点）** で演算・保持し、Flush-to-Zero（`std::abs(z) < 1e-15 ? 0.0 : z`）で Denormal CPUスパイクを防止する。  
- **コンプレッサー（Soft-Knee Feed-Forward & Stereo-Linked）**:  
  - 全有効チャンネルの最大エンベロープを基準に共通のゲインリダクション量を適用し、ステレオ音像の揺れを防止する。  
- **ルックアヘッド・ブリックウォールリミッター**:  
  - 最大 `5.0ms`（192kHz時 `960 samples`）の固定長リングバッファ。先読み窓内の全チャンネル最大ピークを検出し、最終段で `std::clamp(sample, -ceiling_linear, ceiling_linear)` および `std::isfinite(sample)` ガードを適用する。

## 5\. Python 解析アルゴリズム仕様（`kotone-profiler`）

1. **前処理・VAD（有声区間検出）**:  
   - モノラルダウンミックス後、`20ms` フレーム（10msホップ）で短時間エネルギーとZCRを算出。10パーセンタイルを `noise_floor_dbfs` と推定し、`noise_floor_dbfs + 10.0 dB` 以上（かつ `-50 dBFS` 以上）を有声区間として抽出。有効発話時間が `5.0秒` 未満の場合は `ERR_INSUFFICIENT_SPEECH`（終了コード `2`）を返す。  
2. **スペクトル・F0・ダイナミクス・歯擦音解析**:  
   - 4096点ハニング窓FFTによるLTAS算出、1/3オクターブバンド偏差からの最大8バンドPEQ自動フィッティング（ゲイン補正60%スケーリング、`[-6.0, +6.0] dB` 制限）。  
   - 自己相関法による \$F\_0\$ 中央値からの HPF カットオフ算出（\$F\_0 \\times 0.72\$、`65Hz〜110Hz` 制限）。  
   - クレストファクターとRMS分布からのコンプレッサー・メイクアップゲイン算出（囁き声のように入力RMSが低い録音でもターゲットラウドネス `-16 LUFS` 相当へ適切に持ち上がるよう `input_gain_db` と `makeup_gain_db` を協調配分する）。

## 6\. エラーコード一覧

| エラーコード | 発生箇所 | 意味・対処方針 |
| :---- | :---- | :---- |
| `ERR_FILE_NOT_FOUND` | C++ / Python | 指定された音声ファイルまたはプロファイルJSONが存在しない。C++側は安全プロファイルで継続しOBSログに警告出力。 |
| `ERR_FILE_TOO_LARGE` | C++ | プロファイルJSONが `1MB` を超えている。DoS/メモリ枯渇防止のため読込拒否。 |
| `ERR_PROFILE_SAVE_FAILED` | C++ | プロファイルJSONの書き込みまたはアトミックリネームに失敗（ディスク容量不足・権限エラー等）。OBSログへエラー出力し既存ファイルは保護。 |
| `ERR_INVALID_WAV` | Python | WAVヘッダ不正、非対応フォーマット、または破損ファイル。終了コード `1` で終了。 |
| `ERR_INSUFFICIENT_SPEECH` | Python | 有声区間が `5.0秒` 未満で声質解析に十分なデータがない。終了コード `2` で終了。 |
| `ERR_JSON_PARSE_FAILED` | C++ | JSON構文エラー、必須キー欠落、または `NaN`/`Inf` 混入。直前の有効プロファイルを維持（初回は内蔵デフォルト使用）。 |
| `ERR_SCHEMA_UNSUPPORTED` | C++ | `schema_version` のメジャーバージョンが非互換。読込をスキップし安全プロファイル維持。 |
| `ERR_DSP_NON_FINITE` | C++ | DSP演算中に NaN / Inf を検出。該当サンプルを `0.0f` にミュートし、フィルタ状態（`z1, z2`）を自動リセット。 |

