-- ST2110 SDP Loader - VLC Lua extension
-- See docs/ST2110_SDP_Loader_仕様書.md for the full specification.
--
-- Design notes (learned the hard way -- keep this simple):
--   * vlc.playlist.add() already starts playback (that is what distinguishes
--     it from vlc.playlist.enqueue()); do not also call vlc.playlist.play()
--     afterward. A real published extension (ext_audio_loader) relies on
--     exactly this and never calls .play(). A second, redundant play command
--     while a live network item is still mid-open asynchronously is a
--     plausible way to disrupt that open, not a harmless no-op.
--   * Do not force :audio-resampler=soxr. Whether a given VLC build actually
--     ships the soxr module is not something this extension can verify, and
--     forcing an unavailable module is a way to break audio output instead
--     of falling back cleanly.

local USE_CUSTOM_VIDEO_DEFAULT = false   -- 仕様書 §5.3。チェックボックスの初期値。

local dlg, vbox, abox, custom_video_check, status

local function trim(s)
  return s:match("^%s*(.-)%s*$")
end

local function to_uri(path)
  local p = path:gsub("\\", "/")
  if p:match("^%a:/") then
    p = "/" .. p
  end
  p = p:gsub(" ", "%%20")
  return "file://" .. p
end

local function write_sdp(name, text)
  local dir = vlc.config.userdatadir()
  if dir:sub(-1) ~= "/" and dir:sub(-1) ~= "\\" then
    dir = dir .. "/"
  end
  local path = dir .. name

  local f = io.open(path, "w")
  if not f and vlc.io and vlc.io.open then
    f = vlc.io.open(path, "w")
  end
  if not f then
    vlc.msg.err("[ST2110] failed to open " .. path .. " for writing")
    return nil
  end
  f:write(text)
  f:close()
  return path
end

-- fmtp は "key=value;key=value;flag" 形式。値を持たないトークンは真偽フラグとして扱う。
local function parse_fmtp(str)
  local f = {}
  for token in (str .. ";"):gmatch("([^;]+);") do
    token = trim(token)
    if token ~= "" then
      local k, v = token:match("^([%w_]+)=(.*)$")
      if k then
        f[k] = trim(v)
      else
        f[token] = true
      end
    end
  end
  return f
end

-- SDP を「セッションレベル行」と m= ごとの「メディアセクション」に分割する。
local function split_sections(text)
  local session_lines = {}
  local sections = {}
  local current = nil
  for line in (text or ""):gmatch("[^\r\n]+") do
    line = trim(line)
    if line:match("^m=") then
      current = { lines = {} }
      table.insert(current.lines, line)
      table.insert(sections, current)
    elseif current then
      table.insert(current.lines, line)
    else
      table.insert(session_lines, line)
    end
  end
  return session_lines, sections
end

-- "a=group:DUP PRIMARY SECONDARY" から優先する mid 名を返す。PRIMARY があれば
-- それを、無ければ先頭の mid を採用する（2022-7 的な冗長 SDP、§仕様書外の実運用対応）。
local function preferred_mid(session_lines)
  for _, line in ipairs(session_lines) do
    local mids = line:match("^a=group:DUP%s+(.+)$")
    if mids then
      for name in mids:gmatch("%S+") do
        if name:upper() == "PRIMARY" then
          return name
        end
      end
      return mids:match("^(%S+)")
    end
  end
  return nil
end

-- 複数の m= セクションがある場合、優先 mid に一致するものを選ぶ。無ければ先頭。
local function pick_section(session_lines, sections)
  if #sections <= 1 then
    return sections[1]
  end
  local mid = preferred_mid(session_lines)
  if mid then
    for _, sec in ipairs(sections) do
      for _, line in ipairs(sec.lines) do
        if line:match("^a=mid:%s*(%S+)$") == mid then
          return sec
        end
      end
    end
  end
  return sections[1]
end

local function extract_fields(lines)
  local t = {}
  for _, line in ipairs(lines) do
    local addr = line:match("^c=IN%s+IP4%s+([%d%.]+)")
    if addr then t.addr = addr end

    local _, src = line:match("^a=source%-filter:%s*incl%s+IN%s+IP4%s+(%S+)%s+(%S+)")
    if src then t.source = src end

    local media, port, pt = line:match("^m=(%a+)%s+(%d+)%s+RTP/AVP%s+(%d+)")
    if media then
      t.media = media
      t.port = tonumber(port)
      t.pt = tonumber(pt)
    end

    local _, enc, clock, ch = line:match("^a=rtpmap:(%d+)%s+([%w%-]+)/(%d+)/?(%d*)")
    if enc then
      t.rtpmap = {
        encoding = enc,
        clock = tonumber(clock),
        channels = (ch ~= "" and tonumber(ch)) or nil,
      }
    end

    local _, fmtp_rest = line:match("^a=fmtp:(%d+)%s+(.*)$")
    if fmtp_rest then
      t.fmtp = parse_fmtp(fmtp_rest)
    end

    local ptime = line:match("^a=ptime:([%d%.]+)")
    if ptime then t.ptime = tonumber(ptime) end
  end
  return t
end

-- 純粋関数: SDP テキスト -> フィールド table。映像/音声で共用し、NMOS ファサードへ移植可能に保つ（仕様書 §2）。
-- 2022-7 的な冗長 SDP（a=group:DUP + 複数 m=）の場合は PRIMARY 側のセクションのみ取り込む。
local function parse_sdp(text)
  local session_lines, sections = split_sections(text)
  local chosen = pick_section(session_lines, sections)
  if not chosen then
    return extract_fields(session_lines)
  end
  local combined = {}
  for _, l in ipairs(session_lines) do table.insert(combined, l) end
  for _, l in ipairs(chosen.lines) do table.insert(combined, l) end
  return extract_fields(combined)
end

-- 独自 access_demux モジュール（st2110://）向けの MRL 契約（仕様書 §11）。
-- ダイアログの「Use custom 10bit receiver」チェックがオンの時のみ使用。
local function build_st2110_mrl(t)
  if not (t and t.addr and t.port) then
    return nil
  end
  local mrl = string.format("st2110://%s:%d", t.addr, t.port)
  local opts = {}
  if t.source then table.insert(opts, ":st2110-source=" .. t.source) end
  local f = t.fmtp or {}
  if f.width then table.insert(opts, ":st2110-width=" .. f.width) end
  if f.height then table.insert(opts, ":st2110-height=" .. f.height) end
  if f.depth then table.insert(opts, ":st2110-depth=" .. f.depth) end
  if f.sampling then table.insert(opts, ":st2110-sampling=" .. f.sampling) end
  if f.exactframerate then table.insert(opts, ":st2110-fps=" .. f.exactframerate) end
  if f.colorimetry then table.insert(opts, ":st2110-colorimetry=" .. f.colorimetry) end
  if f.interlace then table.insert(opts, ":st2110-interlace") end
  return mrl, opts
end

function descriptor()
  return {
    title = "ST2110 SDP Loader",
    version = "0.2",
    author = "vlc-st2110-player",
    shortdesc = "Paste ST2110 SDP -> play",
    description = "映像/音声の SDP を貼り付けて ST 2110 エッセンスを再生します。",
    capabilities = {},
  }
end

function activate()
  dlg = vlc.dialog("ST2110 SDP Loader")
  dlg:add_label("<b>Video SDP (ST 2110-20)</b>", 1, 1, 4, 1)
  vbox = dlg:add_text_input("", 1, 2, 4, 8)
  dlg:add_label("<b>Audio SDP (ST 2110-30 / AES67)</b>", 1, 10, 4, 1)
  abox = dlg:add_text_input("", 1, 11, 4, 8)
  custom_video_check = dlg:add_check_box(
    "Use custom 10bit receiver (st2110://, requires the C access_demux plugin)",
    USE_CUSTOM_VIDEO_DEFAULT, 1, 19, 4, 1)
  dlg:add_button("Play", play, 1, 20, 1, 1)
  dlg:add_button("Stop", stop, 2, 20, 1, 1)
  dlg:add_button("Clear", clear, 3, 20, 1, 1)
  status = dlg:add_label("", 1, 21, 4, 1)
  dlg:show()
end

function deactivate()
  if dlg then
    dlg:delete()
    dlg = nil
  end
end

function close()
  vlc.deactivate()
end

function play()
  local v = trim(vbox:get_text())
  local a = trim(abox:get_text())
  if v == "" and a == "" then
    status:set_text("SDP を貼り付けてください")
    return
  end

  local main_uri, slave_uri
  local extra_opts = {}
  local labels = {}

  if v ~= "" then
    if custom_video_check:get_checked() then
      local mrl, opts = build_st2110_mrl(parse_sdp(v))
      if not mrl then
        status:set_text("映像 SDP の解析に失敗しました")
        return
      end
      main_uri = mrl
      for _, o in ipairs(opts or {}) do table.insert(extra_opts, o) end
    else
      local p = write_sdp("st2110_video.sdp", v)
      if not p then
        status:set_text("書き込み失敗(video)")
        return
      end
      main_uri = to_uri(p)
    end
    table.insert(labels, "video")
  end

  if a ~= "" then
    local p = write_sdp("st2110_audio.sdp", a)
    if not p then
      status:set_text("書き込み失敗(audio)")
      return
    end
    if main_uri then
      slave_uri = to_uri(p)
    else
      main_uri = to_uri(p)
    end
    table.insert(labels, "audio(SRC)")
  end

  local opts = { ":network-caching=200" }
  for _, o in ipairs(extra_opts) do table.insert(opts, o) end
  if slave_uri then table.insert(opts, ":input-slave=" .. slave_uri) end

  vlc.playlist.clear()
  vlc.playlist.add({ { path = main_uri, options = opts } })
  status:set_text("再生開始: " .. table.concat(labels, " + "))
end

function stop()
  vlc.playlist.stop()
  status:set_text("停止")
end

function clear()
  vbox:set_text("")
  abox:set_text("")
  status:set_text("")
end
