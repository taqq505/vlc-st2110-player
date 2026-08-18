# ST2110 SDP Loader — VLC Lua 拡張 実装仕様書

| 項目 | 内容 |
|---|---|
| 対象成果物 | VLC Lua Extension（単一 `.lua` ファイル） |
| バージョン | 0.1（ドラフト） |
| 実装環境 | 別PC / VS Code + Claude Code |
| ステータス | 実装前レビュー用 |

---

## 1. 目的とスコープ

VLC のダイアログに **SDP をテキストで貼り付けるだけ**で、ST 2110 のエッセンスを再生できる Lua 拡張を作る。

本成果物のスコープ:

- 映像 SDP（ST 2110-20）と音声 SDP（ST 2110-30 / AES67）を**別々の入力欄**で受け取る。
- 両者を **VLC の入力スレーブ機構で 1 本の再生に束ねて**同時に出力する。
- 音声は**フリーラン前提**で、SRC（サンプルレート変換）を効かせてクロックドリフトを吸収する。
- 「Play / Stop / Clear」と状態表示を持つ最小 UI。

**非スコープ（本成果物では作らない）:**

- PTP 同期・ジェネロック（フリーランで割り切る）。
- 独自 C アクセスモジュール（`st2110://`）そのものの実装。ただし将来それへ橋渡しする**フックだけ**用意する（§5.3）。
- NMOS（IS-04/05）連携。将来の別レイヤ。

> 重要な前提: この拡張は**トランスポート非依存の「フロントエンド」**であり、映像・音声そのものをデコードしない。SDP を VLC に手渡すだけで、実際に再生できるかは VLC 側（内蔵 RTP / live555、または将来の独自モジュール）の対応可否に依存する。10bit 4:2:2 というフォーマット自体は VLC で表示可能（出力先は `I422_10L`）であり問題にならない。問題は受信経路の側にあり、ST 2110-20（RFC 4175）の 10bit SMPTE パッキング（5 byte / 2 pixel、v210 とも別物）を解いて `I422_10L` に展開する処理が素の VLC（live555 経由）には事実上存在しない。この「10bit パッキングを解く受信経路」を担うのが将来の独自モジュールであり、それを通せば VLC は 10bit 4:2:2 をそのまま描画する（§5.3 / §11）。

---

## 2. 全体アーキテクチャ内の位置づけ

3 レイヤ構成のうち、本成果物は **(2) 手動 UI レイヤ**にあたる。

1. **エッセンス受信レイヤ** … 独自 C `access_demux` モジュール（`st2110://`）。将来実装。ST 2110-20 をフリーラン受信。
2. **手動 UI レイヤ … 本成果物**。SDP 貼り付け → 再生。
3. **NMOS ファサードレイヤ** … nmos-cpp ベースの外部サイドカー。将来実装。IS-05 アクティベーション時に SDP を VLC へ渡す。

SDP パースと「SDP → VLC 起動」のロジックは 3 レイヤで共有できる設計とする。したがって本拡張の SDP パース関数は、後で NMOS ファサードへ移植可能な純粋関数として切り出すこと。

---

## 3. 対象環境 / 前提

| 項目 | 内容 |
|---|---|
| プレイヤ | VLC media player（3.0.x または 4.0 系。**要確定** → §12） |
| スクリプト種別 | Lua **Extension**（`vlc.dialog` へアクセスできる唯一の種別） |
| 起動 | View / Tools → Plugins and extensions メニューから有効化 |
| 実行モデル | VLC 本体と同一プロセス。`vlc.playlist` で再生制御可能 |

Lua 拡張 API は 3.0.x と 4.0 で概ね共通。**バージョン差はインストール先パス（§9）にのみ影響**し、本拡張のコードには影響しない（独自 C モジュール側は差が出るが、それは別成果物）。

---

## 4. 用語

- **-20 / -30 / -40** … ST 2110 の映像 / 音声 / ANC。本成果物は -20 と -30 を扱う。
- **GPM** … General Packing Mode（2110-20 の標準パッキング。RFC 4175 互換）。
- **SSM** … Source-Specific Multicast。2110 では `a=source-filter` で送信元指定するのが一般的。
- **SRC** … Sample Rate Conversion。ここでは送信側 48k と出力デバイスクロックの ppm 差を吸収する非同期リサンプルを指す。
- **入力スレーブ** … VLC の `:input-slave=` 機構。主入力に従属入力を重ねて同時再生する。

---

## 5. 機能要件

### 5.1 ダイアログ UI

`vlc.dialog` のグリッド（col/row/span）で以下を配置する。行番号は目安。

| 要素 | 種別 | 位置(目安) | 備考 |
|---|---|---|---|
| 見出し「Video SDP (ST 2110-20)」 | label(html可) | 1行目 | |
| 映像 SDP 入力欄 | text_input（複数行） | 2–5行目 | 複数行貼り付け前提。幅 span=4 |
| 見出し「Audio SDP (ST 2110-30 / AES67)」 | label | 6行目 | |
| 音声 SDP 入力欄 | text_input（複数行） | 7–10行目 | |
| Play ボタン | button | 11行目 | `play()` を呼ぶ |
| Stop ボタン | button | 11行目 | `stop()` を呼ぶ |
| Clear ボタン | button | 11行目 | 両欄と状態をクリア |
| 状態ラベル | label | 12行目 | 成否・再生対象を表示 |

- 起動（`activate()`）時にダイアログを生成・表示する。
- 片方の欄だけでも動作すること（映像のみ / 音声のみ可）。

### 5.2 再生動作（`play()`）

1. 両欄のテキストを取得し前後空白を除去。両方空なら状態ラベルにエラーを出して終了。
2. **映像欄が非空**なら、映像を「主入力」の MRL とする（生成方法は §5.3 のモードに従う）。
3. **音声欄が非空**なら、音声 SDP を一時ファイルに書き出して `file://` URI 化し、
   - 映像が主入力として存在する場合 → 音声を **入力スレーブ**にする。
   - 映像が無い場合 → 音声を主入力にする。
4. 再生オプション（§7）を組み立て、`vlc.playlist.clear()` → `add()` → `play()`。
5. 状態ラベルに再生対象（video / audio(SRC) / 両方）を表示。

一時ファイルの保存先は `vlc.config.userdatadir()` 配下とする。ファイル名は固定（例: `st2110_video.sdp` / `st2110_audio.sdp`）で毎回上書きしてよい。

### 5.3 映像ルーティングモード

拡張先頭の設定フラグ `USE_CUSTOM_VIDEO`（真偽値）で切り替える。

- **`false`（既定 / 現時点で動く）**: 映像 SDP をそのまま一時ファイルに書き出し、`file://` URI を主入力にして VLC 内蔵経路（RTP / live555）に委ねる。素の VLC が扱える素材（例: 8bit や live555 が対応する形式、音声）はこれで再生できる。
- **`true`（将来 / 独自モジュール前提）**: 映像 SDP を §5.4 でパースし、`st2110://<grp>:<port>` 形式の MRL を生成して主入力にする。SSM 送信元・width・height・depth 等は `:st2110-*` オプションで渡す。独自 C モジュール実装後に有効化する。

> 音声は常に「SDP ファイルを VLC 内蔵経路へ渡す」方式（`false` 相当）とする。動的ペイロードタイプの音声は `rtpmap`/`fmtp` を伴う SDP が必須のため、`rtp://` 直指定は不可。

### 5.4 SDP パース仕様

SDP から抽出するフィールド。パースは純粋関数 `parse_sdp(text) -> table` として実装し、映像・音声で共用する。

**映像（-20）:**

| SDP 要素 | 例 | 取り出す値 |
|---|---|---|
| `c=IN IP4 <addr>` | `239.10.10.1` | 宛先マルチキャスト |
| `a=source-filter: incl IN IP4 <dest> <src>` | `... 239.10.10.1 192.168.0.10` | SSM 送信元 |
| `m=video <port> RTP/AVP <pt>` | `20000` | 受信ポート / PT |
| `a=rtpmap:<pt> raw/90000` | | ペイロード種別 |
| `a=fmtp:<pt> ...` | `sampling=YCbCr-4:2:2; width=1920; height=1080; exactframerate=60000/1001; depth=10; colorimetry=BT709; PM=2110GPM; SSN=ST2110-20:2017; TP=2110TPN; interlace` | sampling / width / height / depth / exactframerate / colorimetry / PM / TP / interlace(有無) |

`fmtp` の値は `key=value` を `;` 区切りで走査して連想配列化する（`key=[^;]+` を反復）。

**音声（-30 / AES67）:**

| SDP 要素 | 例 | 取り出す値 |
|---|---|---|
| `c=IN IP4 <addr>` | `239.10.10.2` | 宛先マルチキャスト |
| `a=source-filter: ...` | | SSM 送信元 |
| `m=audio <port> RTP/AVP <pt>` | `20010` | ポート / PT |
| `a=rtpmap:<pt> L24/48000/8` | | ビット深度(L16/L24) / サンプルレート / ch 数 |
| `a=ptime:<ms>` | `0.125` / `1` | パケット化時間（参考） |

`ts-refclk` / `mediaclk` はフリーランのため**参照しない**（存在してもよい）。

---

## 6. VLC Lua API 契約（使用する関数）

拡張が依存する VLC Lua の関数・メソッド。実装はこれらの範囲に収めること。

- 拡張メタ関数: `descriptor()`（`title` 必須）、`activate()`、`deactivate()`、`close()`。任意で `input_changed()`, `meta_changed()`。
- ダイアログ: `vlc.dialog(title)` → `:add_label`, `:add_html`, `:add_text_input`, `:add_button(text, callback, col,row,colspan,rowspan)`, `:del_widget`, `:show`, `:hide`, `:delete`, `:update`。
- ウィジェット: `:get_text()`, `:set_text()`。
- 再生: `vlc.playlist.add({ { path=<mrl>, options={ ":opt=val", ... } } })`, `vlc.playlist.play()`, `vlc.playlist.stop()`, `vlc.playlist.clear()`。
- 設定/パス: `vlc.config.userdatadir()`。
- ログ: `vlc.msg.info/dbg/warn/err`。

グリッド座標は **1 始まり**（Lua の慣習に合わせ col/row は 1 から）。

---

## 7. 再生オプション（確定文字列）

`vlc.playlist.add` の item `options`（各要素は `:` 始まりの文字列）。

| オプション | 値 | 目的 |
|---|---|---|
| `:network-caching=` | `200` | RTP ジッタバッファ（ms）。フリーランの初期値。増やすと安定・遅延増 |
| `:audio-resampler=` | `soxr` | 高品質 SRC を明示選択。ドリフト吸収はオーディオ出力が担い、soxr で高品質化 |
| `:input-slave=` | `file:///…/st2110_audio.sdp` | 音声を従属入力として同時再生（映像が主のとき） |

補足:

- SRC の実体は「オーディオ出力が出力デバイスのクロックへ連続リサンプルしてドリフトを吸収する」動作であり、`soxr` はその品質を上げる指定。`soxr` が当該ビルドに無い場合のフォールバックは `src` または `speex_resampler`。
- 複数スレーブが必要になった場合、`:input-slave=` の値は `#` 区切りで連結できる（本成果物では音声 1 系統のみで可）。
- `file://` URI 化: パス区切りを `/` に統一し、Windows のドライブレターは先頭に `/` を補って `file:///C:/…` とする。

---

## 8. エラー処理 / エッジケース

| ケース | 期待動作 |
|---|---|
| 両欄とも空 | 状態ラベルに「SDP を貼り付けてください」。再生しない |
| 映像のみ | 映像を主入力として再生。音声スレーブなし |
| 音声のみ | 音声を主入力として再生 |
| 一時ファイル書き込み失敗 | 状態ラベルにエラー表示し中断。`vlc.msg.err` にも出力 |
| 不正 / 不完全な SDP | `USE_CUSTOM_VIDEO=false` なら VLC 側のエラーに委ねる。`true` の場合はパース失敗を検出し状態表示 |
| 再生中に再度 Play | `playlist.clear()` してから追加するため、直前の再生は停止・置換される |
| Clear | 両欄と状態ラベルを空にする（再生は停止しない） |

---

## 9. インストールと起動

`.lua` を以下へ配置し、VLC 再起動後に Plugins and extensions で有効化する。

| OS | 配置先 |
|---|---|
| Windows | `%APPDATA%\vlc\lua\extensions\` |
| macOS | `~/Library/Application Support/org.videolan.vlc/lua/extensions/` |
| Linux | `~/.local/share/vlc/lua/extensions/` |

フォルダが無ければ作成する。

---

## 10. 受け入れ基準 / テスト

1. **UI**: 拡張を有効化するとダイアログが開き、2 つの入力欄と 3 ボタン・状態ラベルが表示される。
2. **音声**: 既知の AES67 音声 SDP を音声欄に貼って Play → 音が出る。数分再生してもドロップ/歪みが蓄積しない（SRC 動作確認）。
3. **映像（現状経路）**: VLC が扱える映像 SDP を映像欄に貼って Play → 映像が出る。ST 2110-20 の 10bit パッキングは stock 経路に解く係が無いため出ない場合があり、その旨を許容（独自モジュール待ち。フォーマットとしての 10bit 4:2:2 自体は表示可）。
4. **A/V 同時**: 映像＋音声を両欄に貼って Play → 映像に音声が重畳再生される（フリーランのため厳密リップシンクは要求しない）。
5. **Stop / Clear**: それぞれ停止・クリアが機能する。
6. **異常系**: 空 Play・書き込み失敗時に状態ラベルへ適切なメッセージが出る。

---

## 11. スコープ外 / 将来拡張フック

- **独自 `st2110://` C モジュール**: `USE_CUSTOM_VIDEO=true` 時に本拡張が生成する MRL 契約（下記）を受け口とする。
  - 形式: `st2110://<grp>:<port>`
  - オプション: `:st2110-source=<src>`, `:st2110-width=<w>`, `:st2110-height=<h>`, `:st2110-depth=<d>`（必要に応じ `:st2110-sampling=`, `:st2110-interlace` 等を追加）
- **NMOS IS-05 ファサード**: `parse_sdp()` と MRL 生成部を再利用し、アクティベーションコールバックから同じ経路で VLC を起動する。

---

## 12. 決定が必要な事項（Open Questions）

1. **VLC バージョン（3.0.x か 4.0 か）**: インストール先パスの確定に必要。将来の独自モジュール（access_demux か rtp parser か）にも影響。
2. **`USE_CUSTOM_VIDEO` の初期値**: 独自モジュール未実装の現時点では `false` を既定とする想定でよいか。
3. **一時ファイル書き込み API**: 標準 `io.open` が当該環境の拡張サンドボックスで使用可能か要確認。不可なら `vlc.io.open` にフォールバックする実装とする。
4. **音声 L24 対応**: 使用する VLC ビルドが L24/48000 の RTP 受信に対応しているか（未対応なら別途受信手段の検討が必要）。
5. **`soxr` 同梱**: 当該ビルドに `soxr` リサンプラが含まれるか。無ければ `src` へ。

---

## 付録A リファレンス骨格（擬似コード）

実装の出発点。完成コードではなく構造の指針。

```lua
local USE_CUSTOM_VIDEO = false   -- §5.3

function descriptor()
  return { title = "ST2110 SDP Loader", version = "0.1",
           shortdesc = "Paste ST2110 SDP -> play",
           capabilities = {} }
end

local dlg, vbox, abox, status

function activate()
  dlg = vlc.dialog("ST2110 SDP Loader")
  dlg:add_label("<b>Video SDP (ST 2110-20)</b>", 1,1,4,1)
  vbox   = dlg:add_text_input("", 1,2,4,4)
  dlg:add_label("<b>Audio SDP (ST 2110-30 / AES67)</b>", 1,6,4,1)
  abox   = dlg:add_text_input("", 1,7,4,4)
  dlg:add_button("Play",  play,  1,11,1,1)
  dlg:add_button("Stop",  stop,  2,11,1,1)
  dlg:add_button("Clear", clear, 3,11,1,1)
  status = dlg:add_label("", 1,12,4,1)
  dlg:show()
end

function deactivate() end
function close() vlc.deactivate() end

-- helpers: trim / to_uri / write_sdp(name,text) / parse_sdp(text) / build_st2110_mrl(t)

function play()
  local v, a = trim(vbox:get_text()), trim(abox:get_text())
  if v == "" and a == "" then status:set_text("SDP を貼り付けてください"); return end

  local main_uri, slave_uri
  if v ~= "" then
    if USE_CUSTOM_VIDEO then
      main_uri = build_st2110_mrl(parse_sdp(v))
    else
      local p = write_sdp("st2110_video.sdp", v)
      if not p then status:set_text("書き込み失敗(video)"); return end
      main_uri = to_uri(p)
    end
  end
  if a ~= "" then
    local p = write_sdp("st2110_audio.sdp", a)
    if not p then status:set_text("書き込み失敗(audio)"); return end
    if main_uri then slave_uri = to_uri(p) else main_uri = to_uri(p) end
  end

  local opts = { ":network-caching=200", ":audio-resampler=soxr" }
  if slave_uri then table.insert(opts, ":input-slave=" .. slave_uri) end

  vlc.playlist.clear()
  vlc.playlist.add({ { path = main_uri, options = opts } })
  vlc.playlist.play()
  status:set_text("再生開始")
end

function stop()  vlc.playlist.stop(); status:set_text("停止") end
function clear() vbox:set_text(""); abox:set_text(""); status:set_text("") end
```

## 付録B 参考資料

- VLC Lua Docs — Dialog / Extension Scripts（`vlc.dialog`, 拡張メタ関数）
- VLC `--input-slave`（複数入力の同時再生。実験的だが同期メディア用途で動作）
- VLC HTTP requests API（本成果物では未使用。将来の外部制御用）
- RFC 4175 / SMPTE ST 2110-20（-20 GPM のペイロード）、ST 2110-30 / AES67（音声）
