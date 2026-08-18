# vlc-st2110 受信モジュール（access_demux）実装仕様書

| 項目 | 内容 |
|---|---|
| 対象成果物 | VLC C プラグイン（`libst2110_plugin.so` / `.dll` / `.dylib`） |
| モジュール種別 | `access_demux`（`st2110://` を処理） |
| バージョン | 0.1（ドラフト） |
| 実装環境 | 別PC / VS Code + Claude Code |
| ステータス | 実装前レビュー用 |

---

## 0. この仕様書の位置づけ（重要）

前成果物「ST2110 SDP Loader（Lua 拡張）」は **UI と SDP 受け渡しのみ**で、映像そのものはデコードしない。実際に ST 2110-20 を受信して絵にする処理は VLC では **C の受信モジュール側**の責務であり、Lua からは実装できない。本仕様書はその**受信本体**を定義する。

完成時の全体像:

- **本モジュール（C, 新規）** … `st2110://` を受け、-20 映像を受信・デパケット化・展開して VLC の映像 ES として出力。**これが「映が出る」の本体**。
- **Lua 拡張（既存）** … SDP を貼り付け、映像は `st2110://` MRL を生成して本モジュールへ、音声は SDP を `input-slave` で VLC 内蔵経路へ渡す。`USE_CUSTOM_VIDEO=true` に切り替えて連携する。
- **VLC 内蔵音声経路（既存）** … -30 / AES67 音声を担当。本モジュールは映像のみ。

この 3 者が揃って初めて「VLC 単体で貼るだけで映像＋音声」が成立する。本仕様書のスコープは**映像受信モジュール**に限定する。

---

## 1. 目的とスコープ

`st2110://<group>:<port>` を開くと、指定マルチキャストの ST 2110-20 非圧縮映像を受信し、フリーランで VLC の映像出力に描画する C モジュールを実装する。

### スコープ（本バージョンで実装）

- ST 2110-20、**YCbCr 4:2:2 / 10bit / GPM（2110GPM）** の受信。
- RTP / UDP マルチキャスト受信（IGMP。SSM 対応）。
- RFC 4175 / 2110-20 ペイロードヘッダのデパケット化とフレーム再構成。
- 10bit 4:2:2 パッキング（5 byte / 2 pixel）→ **`VLC_CODEC_I422_10L`** への展開。
- **フリーラン**（PTP 同期なし。PC 内部でフレーム確定＝ソフト FS）。
- プログレッシブを第一級サポート。インターレースは定義するが任意（§5.6）。

### 非スコープ（将来／別成果物）

- BPM パッキング、4:4:4 / RGB、8/12/16bit（depth はパラメータ化して拡張余地を残す）。
- 音声（-30）・ANC（-40）。
- PTP / SyncE 同期、ジェネロック。
- NMOS（IS-04/05）連携。

---

## 2. 対象環境 / 前提

| 項目 | 内容 |
|---|---|
| VLC バージョン | **3.0.x を第一ターゲット**（`access_demux` capability が安定）。4.0 系は差分あり → §12 |
| ビルド | out-of-tree。`pkg-config --cflags/--libs vlc-plugin` |
| 依存 | libvlccore（SDK）。追加ライブラリ不要（標準 C とソケットのみ） |
| 対象帯域 | 1080p59.94 10bit ≈ 2.5 Gbps を処理できること |

> 3.0.x と 4.0 で `access_demux` 登録・時間 API・一部 es_out API が異なる。本書は 3.0.x の名称で記述し、4.0 差分は該当箇所に注記する。実装着手前に**ターゲット VLC バージョンを確定**すること（§12-1）。

---

## 3. 用語

- **GPM** … General Packing Mode（2110-20 標準。RFC 4175 互換の連続パッキング）。
- **pgroup** … ピクセルグループ。10bit 4:2:2 では 2 pixel = 4 sample × 10bit = 40bit = **5 octet**。
- **SSM** … Source-Specific Multicast。`st2110-source` で送信元を指定。
- **ESN** … Extended Sequence Number（ペイロード先頭 16bit。RTP seq 下位 16bit と合わせ 32bit）。
- **Marker（M）ビット** … RTP ヘッダの M。フレーム最終パケットで 1。
- **フリーラン** … 送信側クロックに同期せず、受信フレームを自クロックで送出する方式。

---

## 4. 起動インタフェース（MRL とオプション）

本モジュールは SDP を直接受け取らない（SDP デマルチプレクサ経路とは別）。**必要パラメータは MRL オプションで受け取る**。これらは Lua 拡張の `parse_sdp()` が SDP から抽出して生成する（前成果物 §11 の契約と一致させること）。

### MRL

```
st2110://<group>:<port>
```

`<group>` は宛先マルチキャストアドレス、`<port>` は受信ポート。`p_demux->psz_location` から取得し、末尾の `:` で分割する。

### オプション（モジュールで宣言し `var_Inherit*` で読む）

| オプション | 型 | 既定 | 用途 |
|---|---|---|---|
| `:st2110-source` | string | "" | SSM 送信元アドレス。空なら ASM |
| `:st2110-width` | int | 1920 | 有効幅（pixel） |
| `:st2110-height` | int | 1080 | 有効高（line） |
| `:st2110-depth` | int | 10 | ビット深度（本版は 10 のみ検証） |
| `:st2110-sampling` | string | "YCbCr-4:2:2" | サンプリング（本版は 4:2:2 のみ） |
| `:st2110-fps` | string | "60000/1001" | フレームレート（`num/den`） |
| `:st2110-interlace` | bool | false | インターレースフラグ |
| `:st2110-colorimetry` | string | "BT709" | 色域（BT709 / BT2020） |
| `:st2110-tcs` | string | "SDR" | 伝達特性（SDR / PQ / HLG） |

オプションが範囲外・未対応値の場合は §8 に従う。

---

## 5. 機能要件

### 5.1 オープン（`Open`）

1. `psz_location` から `group` / `port` を、オプションから各パラメータを取得。
2. マルチキャスト受信ソケットを開く（§5.2）。
3. 出力 ES フォーマットを構築（§5.5）し `es_out_Add`。
4. `p_demux->pf_demux = Demux; p_demux->pf_control = Control;` を設定。
5. フレームバッファ・状態を確保。失敗時は確保済み資源を解放し `VLC_EGENERIC`。

### 5.2 ソケット / マルチキャスト参加

- `net_OpenDgram(p_this, group, port, source, 0, IPPROTO_UDP)` で開く。
  - `source` が非空 → IGMPv3 SSM（S,G）参加。空 → ASM。
- 受信バッファを大きく取る（`SO_RCVBUF` 拡大。目安 16〜64 MiB）。損失低減の要。
- 受信は `net_Read`（p_this を渡し、オブジェクト破棄で中断可能にする）。

### 5.3 受信ループ（`Demux`）— デパケット化とフレーム再構成

`Demux()` は VLC 入力スレッドから繰り返し呼ばれる。**1 フレーム完成するまでパケットを読み、完成したら送出して `VLC_DEMUXER_SUCCESS` を返す**。致命的エラーで `VLC_DEMUXER_EGENERIC`、EOF 相当は返さない（ライブ）。

各パケットの処理:

1. **RTP ヘッダ解析**（12 + CC×4 byte。X ビットがあれば拡張ヘッダ長を読んでスキップ）。M ビットと RTP timestamp、seq を保持。
2. **ペイロードヘッダ解析**: 先頭 2 byte の ESN をスキップ。続く**ラインヘッダのリスト**を、各 6 byte で解析:
   - `Length(16)` … このライン片のオクテット数。
   - `F(1) | Line No(15)` … フィールド識別＋ライン番号。
   - `C(1) | Offset(15)` … 継続ビット＋ライン内ピクセルオフセット。
   - `C==1` の間、次の 6 byte ヘッダを続けて読む。`C==0` で終端。
3. 終端後、各ライン片の**ピクセルデータ**が宣言順に連結されている。各片を長さ `Length` 分だけフレームバッファへ書き込む:
   - 行 = `Line No`（インターレース時は §5.6）。
   - 行内バイトオフセット = `(Offset / 2) × 5`（10bit 4:2:2 の pgroup 換算。Offset は偶数前提）。
4. **フレーム境界判定**: 次のいずれかでフレーム確定とする。
   - M ビット = 1（正規の境界）。
   - RTP timestamp が直前と変化（M パケット欠落時の保険）。
5. フレーム確定時: フレームバッファを `I422_10L` へ展開（§5.4）した `block_t` を生成し、PTS を付与（§5.7）して `es_out_Send`。バッファを次フレーム用にリセットして `return VLC_DEMUXER_SUCCESS`。

### 5.4 ピクセル展開（10bit 4:2:2 → I422_10L）

pgroup（5 byte）を Cb / Y0 / Cr / Y1（各 10bit、ビッグエンディアン詰め）に分解し、`I422_10L`（プレーナ、10bit を 16bit リトルエンディアンに格納）へ書く。

- 分解:
  - `Cb = (b0 << 2) | (b1 >> 6)`
  - `Y0 = ((b1 & 0x3f) << 4) | (b2 >> 4)`
  - `Cr = ((b2 & 0x0f) << 6) | (b3 >> 2)`
  - `Y1 = ((b3 & 0x03) << 8) | b4`
- 格納: Y0→Y 面の列 x、Y1→列 x+1、Cb→U 面の列 x/2、Cr→V 面の列 x/2（4:2:2＝クロマ半幅・全高）。各サンプルは 16bit LE の下位 10bit に値を置く。

ブリングアップ時の高速確認用に、各サンプルを `>> 2` で 8bit 化して `VLC_CODEC_UYVY` に出す簡易パスを**任意**で用意してよい（§10-3）。

### 5.5 出力 ES フォーマット

`es_format_Init(&fmt, VIDEO_ES, VLC_CODEC_I422_10L)` の上で:

- `video.i_width` / `i_height`（アライメント考慮）、`i_visible_width` / `i_visible_height` = width / height。
- `i_sar_num` / `i_sar_den` = 1 / 1（特記なき限り）。
- `i_frame_rate` / `i_frame_rate_base` = fps の num / den。
- `primaries` / `transfer` / `space` / `color_range` を colorimetry / tcs から設定（§5.8）。

### 5.6 インターレース（任意）

- `st2110-interlace=true` の場合、`F` ビットでフィールドを識別し、フィールドラインを全フレームへインターリーブ配置する。ライン番号の意味づけは 2110-20 に従う。
- 本版では**プログレッシブを必須、インターレースは任意実装**とし、未実装なら明示的に非対応を返してよい。ライン番号基点と併せ実機 pcap で検証すること（§12-2）。

### 5.7 タイミング / フリーラン FS

- PTP 非同期。各完成フレームに `PTS = 現在時刻 + jitter_delay` を付与して送出（3.0.x は `mdate()`＋`CLOCK_FREQ`、4.0 は `vlc_tick_now()`＋`VLC_TICK_0`）。
- `jitter_delay` は `network-caching`（既定 200ms 目安）から算出。vout がこの PTS に基づき表示レートへ間引き／繰り返しを行う＝**内部 FS**。
- RTP timestamp は**フレーム境界検出にのみ**使用し、表示タイミングには用いない。

### 5.8 色設定マッピング

| 入力 | primaries / space | transfer |
|---|---|---|
| colorimetry=BT709 | BT709 | tcs=SDR → BT709 |
| colorimetry=BT2020 | BT2020 | tcs=PQ → SMPTE2084 / HLG → ARIB B67 / SDR → BT2020 |

`color_range` は既定でナローレンジ（video range）。

### 5.9 制御（`Control`）

| クエリ | 応答 |
|---|---|
| `DEMUX_CAN_PAUSE` | false |
| `DEMUX_CAN_SEEK` | false |
| `DEMUX_CAN_CONTROL_PACE` | true |
| `DEMUX_GET_PTS_DELAY` | `network-caching` 由来の値 |
| `DEMUX_GET_TIME` / `DEMUX_GET_LENGTH` | 非対応（`VLC_EGENERIC`） |
| `DEMUX_SET_PAUSE_STATE` | no-op で成功 |

### 5.10 クローズ（`Close`）

ソケットクローズ、フレームバッファ・`p_sys` 解放。受信スレッドを設けた場合は停止・join。

---

## 6. 内部設計指針

- **フレームバッファ再利用**: パケット毎の malloc を避け、パックドフレームバッファ（`height × stride_packed`、`stride_packed = (width/2)×5`）を 1 枚使い回す。確定時に `I422_10L` の `block_t` へ展開。
- **損失方針**: seq/ESN（32bit）でギャップ検出。欠落があってもベストエフォートで書き込み、境界で送出（部分フレーム許容）。極端に埋まらないフレームは破棄してよい（閾値は実装裁量、ログ出力）。
- **スレッドモデル**: 既定は `Demux()` 内ブロッキング受信で可。高帯域での取りこぼし対策として、専用受信スレッド＋リングバッファ（`recvmmsg` バッチ受信）を**任意**の強化として設計に織り込む。
- **エンディアン**: ペイロードは全てビッグエンディアン。ヘッダ解析は明示シフトで行い、ホスト依存の型パンを避ける。

---

## 7. ビルド / インストール

- pkg-config: `vlc-plugin`。コンパイル定義: `-D__PLUGIN__ -DMODULE_STRING=\"st2110\"`、`-std=gnu99 -DPIC`。
- 出力: `libst2110_plugin.so`（Win: `.dll` / mac: `.dylib`）。
- 配置: VLC の plugins ディレクトリ（例 Linux `/usr/lib/…/vlc/plugins/access/`）、または開発時は `VLC_PLUGIN_PATH` で指す。
- 確認: `vlc --list | grep -i st2110` に出ること、`vlc -vvv st2110://…` で本モジュールが選択されること。
- **ABI 注意**: ビルド VLC と実行 VLC のバージョン一致必須（`vlc_entry__<version>` がロード可否を決める）。

Makefile 雛形は付録 B。

---

## 8. エラー処理 / エッジケース

| ケース | 期待動作 |
|---|---|
| MRL に group/port 無し | `Open` で `VLC_EGENERIC`、`msg_Err` |
| ソケット / 参加失敗 | `Open` で失敗、資源解放 |
| depth ≠ 10 / sampling ≠ 4:2:2 | 本版は非対応として `Open` で失敗しログ（将来拡張点） |
| Offset が奇数 / pgroup 非整合 | 当該ライン片を破棄しログ、処理継続 |
| ライン番号が height 超過 | 当該片を破棄しログ、処理継続（バッファ溢れ防止） |
| M パケット欠落 | timestamp 変化で境界確定（§5.3-4） |
| 長時間パケット無し | `net_Read` はオブジェクト破棄で中断可能。停止操作に応答すること |

---

## 9. Lua 拡張との連携（受け口契約）

前成果物の Lua 拡張は `USE_CUSTOM_VIDEO=true` 時、`parse_sdp()` の結果から次を生成して本モジュールを呼ぶ。**オプション名を本仕様と一致させること**。

```
st2110://<grp>:<port>
  :st2110-source=<src> :st2110-width=<w> :st2110-height=<h>
  :st2110-depth=<d> :st2110-sampling=<s> :st2110-fps=<num/den>
  :st2110-colorimetry=<c> [:st2110-interlace]
```

音声は従来どおり SDP ファイルを `:input-slave=` で VLC 内蔵経路へ渡す（本モジュールは関与しない）。

---

## 10. 受け入れ基準 / テスト

1. **ロード**: `vlc --list` に `st2110` の access_demux が現れる。
2. **単体再生**: 既知の 1080p 10bit 4:2:2 GPM 送出（ジェネレータ／自作 sender）に対し `vlc st2110://<grp>:<port> :st2110-width=1920 :st2110-height=1080 :st2110-depth=10` で**正しい映像が出る**（色・幾何が正常）。
3. **簡易パス（任意）**: 8bit UYVY 簡易展開でも絵が出ることを先に確認し、10bit 展開へ移行してよい。
4. **フリーラン安定性**: 数分連続再生でクラッシュ・単調増加する破綻が無い。軽微なパケット損失で致命的破綻しない。
5. **SSM**: `st2110-source` 指定時に (S,G) 参加で受信できる。
6. **UI 連携**: Lua 拡張（`USE_CUSTOM_VIDEO=true`）から映像＋音声を貼って、映像は本モジュール、音声は内蔵経路で同時再生。
7. **停止応答**: 再生停止・ウィンドウ閉じで速やかに `Close` される。

---

## 11. パフォーマンス要件

- 1080p59.94 10bit（≈2.5 Gbps、1 フレーム約 5 MB、pgroup 約 200 万/フレーム）を安定処理。
- 対策: `SO_RCVBUF` 拡大、フレームバッファ再利用、（任意で）`recvmmsg` バッチ・専用受信スレッド・リングバッファ。
- 展開ループはホットパス。分岐を減らし、可能なら SIMD 化を将来検討（本版はスカラで可）。

---

## 12. 決定が必要な事項（Open Questions）

1. **VLC バージョン（3.0.x か 4.0 か）**: 登録マクロ・時間 API（`mdate` vs `vlc_tick_now`）・access/demux 統合に影響。**最優先で確定**。
2. **ライン番号基点**: 0 始まり / 1 始まり / ラスタ準拠のいずれか。送出機依存のため**実機 pcap を Wireshark の 2110-20 ディセクタで確認**して確定。
3. **出力コーデック**: `I422_10L` 本命で確定してよいか（別案 v210 / 8bit UYVY 簡易パス）。
4. **インターレース**: 本版で実装するか、非対応返却に留めるか。
5. **損失時のフレーム破棄閾値**: 部分フレームを出す下限（例: 有効ライン率）をどう決めるか。

---

## 付録A リファレンス骨格（擬似コード）

構造の指針。完成コードではない。

```c
/* --- module registration (VLC 3.0.x) --- */
vlc_module_begin()
    set_shortname("ST2110")
    set_description("SMPTE ST 2110-20 uncompressed video receiver")
    set_capability("access_demux", 0)
    set_callbacks(Open, Close)
    add_shortcut("st2110")
    set_category(CAT_INPUT); set_subcategory(SUBCAT_INPUT_ACCESS)
    add_string("st2110-source", "", "SSM source", NULL, true)
    add_integer("st2110-width", 1920, "Width", NULL, true)
    add_integer("st2110-height", 1080, "Height", NULL, true)
    add_integer("st2110-depth", 10, "Bit depth", NULL, true)
    /* ... 他オプション ... */
vlc_module_end()

/* --- 6-byte line header --- */
typedef struct { uint16_t len, line, offset; uint8_t field, cont; } line_hdr_t;

/* RTP + 2110-20 ペイロードヘッダを解析、ピクセルデータ先頭を返す */
static const uint8_t *parse_2110(const uint8_t *p, size_t n,
                                 line_hdr_t *lines, int *n_lines, int *marker)
{
    int cc = p[0] & 0x0f;
    *marker = (p[1] & 0x80) != 0;
    const uint8_t *q = p + 12 + cc * 4;
    if (p[0] & 0x10) { int e = (q[2]<<8)|q[3]; q += 4 + e*4; } /* X */
    q += 2;                                                    /* ESN */
    int k = 0;
    for (;;) {
        uint16_t len = (q[0]<<8)|q[1];
        uint16_t fl  = (q[2]<<8)|q[3];
        uint16_t co  = (q[4]<<8)|q[5];
        lines[k].len=len; lines[k].field=fl>>15; lines[k].line=fl&0x7fff;
        lines[k].cont=co>>15; lines[k].offset=co&0x7fff;
        q += 6; k++;
        if (!lines[k-1].cont) break;
    }
    *n_lines = k;
    return q;   /* この後 lines[i].len ずつピクセルデータが連結 */
}

/* pgroup(5B) → Cb,Y0,Cr,Y1(各10bit) */
static inline void unpack_pgroup(const uint8_t *p,
    uint16_t *cb, uint16_t *y0, uint16_t *cr, uint16_t *y1)
{
    *cb = (p[0]<<2) | (p[1]>>6);
    *y0 = ((p[1]&0x3f)<<4) | (p[2]>>4);
    *cr = ((p[2]&0x0f)<<6) | (p[3]>>2);
    *y1 = ((p[3]&0x03)<<8) |  p[4];
}

static int Demux(demux_t *demux) {
    /* net_Read → parse_2110 → framebuffer へ書込 →
       marker or timestamp変化で確定 → I422_10L block を作り
       i_pts = mdate()+delay で es_out_Send → return VLC_DEMUXER_SUCCESS */
}

static int Control(demux_t *demux, int q, va_list a) {
    switch (q) {
      case DEMUX_CAN_PAUSE:        *va_arg(a,bool*)=false; return VLC_SUCCESS;
      case DEMUX_CAN_SEEK:         *va_arg(a,bool*)=false; return VLC_SUCCESS;
      case DEMUX_CAN_CONTROL_PACE: *va_arg(a,bool*)=true;  return VLC_SUCCESS;
      /* GET_PTS_DELAY 等 */
      default: return VLC_EGENERIC;
    }
}
```

## 付録B Makefile 雛形

```make
PREFIX   = /usr
CC       = cc
CFLAGS   = -std=gnu99 -O2 -Wall -Wextra -DPIC $(shell pkg-config --cflags vlc-plugin)
LIBS     = $(shell pkg-config --libs vlc-plugin)
CPPFLAGS = -D__PLUGIN__ -DMODULE_STRING=\"st2110\"
plugindir = $(PREFIX)/lib/vlc/plugins/access

libst2110_plugin.so: st2110.o
	$(CC) -shared -o $@ $< $(LIBS)

st2110.o: st2110.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

install: libst2110_plugin.so
	install -D $< $(plugindir)/$<

clean:
	rm -f *.o *.so
```

## 付録C 参考資料

- SMPTE ST 2110-20（非圧縮アクティブビデオ）、RFC 4175（Uncompressed Video RTP Payload）
- VLC hacking / module API（`access_demux`, `es_out`, `net_OpenDgram`, `var_Inherit*`）
- VLC fourcc `I422_10L`（10bit 4:2:2 プレーナ）
- ライン番号・パッキング検証: Wireshark ST 2110-20 ディセクタ
