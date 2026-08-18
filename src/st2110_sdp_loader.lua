local USE_CUSTOM_VIDEO = false   -- 仕様書 §5.3

local dlg, vbox, abox, status

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

-- 純粋関数: SDP テキスト -> フィールド table。映像/音声で共用し、NMOS ファサードへ移植可能に保つ（仕様書 §2）。
local function parse_sdp(text)
  local t = {}
  for line in (text or ""):gmatch("[^\r\n]+") do
    line = trim(line)

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

-- 独自 access_demux モジュール（st2110://）向けの MRL 契約（仕様書 §11）。USE_CUSTOM_VIDEO=true でのみ使用。
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
    version = "0.1",
    author = "vlc-st2110-player",
    shortdesc = "Paste ST2110 SDP -> play",
    description = "映像/音声の SDP を貼り付けて ST 2110 エッセンスを再生します。",
    capabilities = {},
  }
end

function activate()
  dlg = vlc.dialog("ST2110 SDP Loader")
  dlg:add_label("<b>Video SDP (ST 2110-20)</b>", 1, 1, 4, 1)
  vbox = dlg:add_text_input("", 1, 2, 4, 4)
  dlg:add_label("<b>Audio SDP (ST 2110-30 / AES67)</b>", 1, 6, 4, 1)
  abox = dlg:add_text_input("", 1, 7, 4, 4)
  dlg:add_button("Play", play, 1, 11, 1, 1)
  dlg:add_button("Stop", stop, 2, 11, 1, 1)
  dlg:add_button("Clear", clear, 3, 11, 1, 1)
  status = dlg:add_label("", 1, 12, 4, 1)
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
    if USE_CUSTOM_VIDEO then
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

  local opts = { ":network-caching=200", ":audio-resampler=soxr" }
  for _, o in ipairs(extra_opts) do table.insert(opts, o) end
  if slave_uri then table.insert(opts, ":input-slave=" .. slave_uri) end

  vlc.playlist.clear()
  vlc.playlist.add({ { path = main_uri, options = opts } })
  vlc.playlist.play()
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
