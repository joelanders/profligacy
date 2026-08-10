local outdir = assert(os.getenv("PROFLIGACY_REAPER_OUTDIR"))
local report_path = outdir .. "/stage1_report.txt"
local chunk_path = outdir .. "/stage1_track_chunk.txt"
local project_path = outdir .. "/profligacy_smoke.rpp"

local function write_file(path, body)
  local f = assert(io.open(path, "wb")); f:write(body); f:close()
end
local function finish(ok, detail, fx_name)
  write_file(report_path, table.concat({
    "status=" .. (ok and "PASS" or "FAIL"), "detail=" .. detail,
    "fx_name=" .. (fx_name or ""), "project=" .. project_path,
    "chunk=" .. chunk_path, "",
  }, "\n"))
  reaper.Main_OnCommand(40004, 0)
end

local function main()
  reaper.InsertTrackAtIndex(0, true)
  local track = reaper.GetTrack(0, 0)
  if not track then finish(false, "could not create track"); return end
  local fx = reaper.TrackFX_AddByName(track, "VST3i: Profligacy", false, -1)
  if fx < 0 then fx = reaper.TrackFX_AddByName(track, "VST3: Profligacy", false, -1) end
  if fx < 0 then finish(false, "could not instantiate staged Profligacy VST3"); return end
  local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
  local item = reaper.CreateNewMIDIItemInProj(track, 12.0, 17.0, false)
  local take = item and reaper.GetActiveTake(item)
  if not take then finish(false, "could not create MIDI take", fx_name); return end
  local first = reaper.MIDI_GetPPQPosFromProjTime(take, 12.25)
  local last = reaper.MIDI_GetPPQPosFromProjTime(take, 16.25)
  if not reaper.MIDI_InsertNote(take, false, false, first, last, 0, 60, 110, false) then
    finish(false, "could not insert MIDI note", fx_name); return
  end
  reaper.MIDI_Sort(take)
  local ok, chunk = reaper.GetTrackStateChunk(track, "", false)
  if not ok or #chunk < 1000 then finish(false, "track state chunk missing", fx_name); return end
  write_file(chunk_path, chunk)
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 24.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FILE", outdir, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "reaper_render", true)
  reaper.Main_SaveProjectEx(0, project_path, 8)
  finish(true, "scan+instantiate+midi+state+save", fx_name)
end
local ok, err = xpcall(main, debug.traceback)
if not ok then finish(false, "Lua error: " .. tostring(err)) end
