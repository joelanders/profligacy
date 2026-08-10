local outdir = assert(os.getenv("PROFLIGACY_REAPER_OUTDIR"))
local report_path = outdir .. "/stage2_report.txt"
local chunk_path = outdir .. "/stage2_loaded_track_chunk.txt"
local function write_file(path, body)
  local f = assert(io.open(path, "wb")); f:write(body); f:close()
end
local function finish(ok, detail, first_name, second_name)
  write_file(report_path, table.concat({
    "status=" .. (ok and "PASS" or "FAIL"), "detail=" .. detail,
    "restored_fx_name=" .. (first_name or ""),
    "second_fx_name=" .. (second_name or ""), "chunk=" .. chunk_path, "",
  }, "\n"))
  reaper.Main_OnCommand(40004, 0)
end
local track = reaper.GetTrack(0, 0)
if not track or reaper.TrackFX_GetCount(track) < 1 then finish(false, "saved FX missing"); return end
local _, first_name = reaper.TrackFX_GetFXName(track, 0, "")
if not string.find(string.lower(first_name or ""), "profligacy", 1, true) then
  finish(false, "restored FX identity mismatch", first_name); return
end
local ok, chunk = reaper.GetTrackStateChunk(track, "", false)
if not ok or #chunk < 1000 then finish(false, "restored state chunk missing", first_name); return end
write_file(chunk_path, chunk)
reaper.InsertTrackAtIndex(1, true)
local second = reaper.GetTrack(0, 1)
local second_fx = reaper.TrackFX_AddByName(second, "VST3i: Profligacy", false, -1)
if second_fx < 0 then second_fx = reaper.TrackFX_AddByName(second, "VST3: Profligacy", false, -1) end
if second_fx < 0 then finish(false, "could not instantiate second instance", first_name); return end
local _, second_name = reaper.TrackFX_GetFXName(second, second_fx, "")
finish(true, "reopen+state+second-instance", first_name, second_name)
