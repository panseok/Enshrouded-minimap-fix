-- One-shot exporter for the shroud ("FogZone") layer of the in-game map.
-- Writes raw texture bytes only (no decoding: EML's decoder does not handle
-- BC5/BC7 reliably); tools/map-render decodes them offline.
-- Every game call is wrapped in pcall: an uncaught error here would take down
-- the whole mod loader, not just this mod.
local MAP_GUID = "01bcbd07-bdbf-41c0-9999-5d58fb1a3aa1"
local PREFIX = "minimap_fog_exporter"
local lines = {}

local function log_line(text)
    pcall(function()
        local line = "[MinimapFogExporter] " .. tostring(text)
        print(line)
        lines[#lines + 1] = line
    end)
end

local function safe_call(label, fn)
    local ok, result = pcall(fn)
    if not ok then
        log_line(label .. " failed: " .. tostring(result))
        return nil
    end
    return result
end

local function field(value, key)
    local ok, result = pcall(function() return value[key] end)
    if ok then
        return result
    end
    return nil
end

local function wanted(name)
    local lower = string.lower(tostring(name or ""))
    return string.find(lower, "fog", 1, true) ~= nil
end

local function dump(label, value, depth)
    depth = depth or 0
    if depth > 2 then
        return
    end
    pcall(function()
        for key, v in pairs(value) do
            local t = type(v)
            if t == "table" or t == "userdata" then
                log_line(string.rep("  ", depth) .. label .. "." .. tostring(key) .. " (" .. t .. ")")
                dump(label .. "." .. tostring(key), v, depth + 1)
            else
                log_line(string.rep("  ", depth) .. label .. "." .. tostring(key) .. " = " .. tostring(v))
            end
        end
    end)
end

log_line("version 0.1.0")

local map = safe_call("get_resource keen::UiMapResource", function()
    return game.assets.get_resource(MAP_GUID, "keen::UiMapResource")
end)
if map ~= nil then
    dump("UiMapResource", field(map, "data") or map)
end

local count = 0
for part = 0, 260 do
    local resource = safe_call("get_resource part " .. tostring(part), function()
        return game.assets.get_resource(MAP_GUID, "keen::UiTextureResource", part)
    end)
    local data = resource ~= nil and field(resource, "data") or nil
    local name = data ~= nil and field(data, "debugName") or nil
    if data ~= nil and wanted(name) then
        local size = field(data, "size")
        local width = size ~= nil and field(size, "x") or nil
        local height = size ~= nil and field(size, "y") or nil
        local content = safe_call("get_content part " .. tostring(part), function()
            return game.assets.get_content(field(data, "data"))
        end)
        local raw = content ~= nil and safe_call("read_data part " .. tostring(part), function()
            return content:read_data()
        end) or nil
        if raw ~= nil then
            local base = PREFIX .. "/part_" .. string.format("%03d", part)
            local ok = safe_call("export part " .. tostring(part), function()
                io.export(base .. ".raw", raw)
                return true
            end)
            if ok then
                count = count + 1
                log_line("part " .. tostring(part) ..
                    " name=" .. tostring(name) ..
                    " size=" .. tostring(width) .. "x" .. tostring(height) ..
                    " format=" .. tostring(field(data, "format")) ..
                    " levels=" .. tostring(field(data, "levelCount")) ..
                    " file=" .. base .. ".raw")
            end
        end
    end
end

log_line("exported " .. tostring(count) .. " fog textures")
pcall(function()
    io.export(PREFIX .. "/export-log.txt", table.concat(lines, "\n"))
end)

return {}
