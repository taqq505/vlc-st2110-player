-- Baseline test build: identical to the very first working version of this
-- extension, with NONE of the later additions (SDP text caching, custom-
-- video checkbox, network-caching UI field/parsing). Used only to A/B test
-- whether those additions are responsible for the current "never starts
-- playing" symptom, by comparing against known-original behavior in the
-- current environment. Not meant to be kept -- delete once the comparison
-- is done.

local USE_CUSTOM_VIDEO = false

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

function descriptor()
  return {
    title = "ST2110 SDP Loader (baseline)",
    version = "0.1",
    shortdesc = "Paste ST2110 SDP -> play",
    capabilities = {},
  }
end

function activate()
  dlg = vlc.dialog("ST2110 SDP Loader (baseline)")
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
  local v, a = trim(vbox:get_text()), trim(abox:get_text())
  if v == "" and a == "" then status:set_text("SDP を貼り付けてください"); return end

  local main_uri, slave_uri
  if v ~= "" then
    local p = write_sdp("st2110_video.sdp", v)
    if not p then status:set_text("書き込み失敗(video)"); return end
    main_uri = to_uri(p)
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
