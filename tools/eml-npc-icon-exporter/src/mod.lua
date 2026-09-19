-- One-shot exporter for the world map's dynamic icons (NPC, NPC bed, animal, flame altar)
-- that live outside the MapMarkerRegistry, plus a name list of every UiTextureResource.
-- Everything is wrapped in pcall: an uncaught error takes down the whole mod loader.
local PREFIX = "minimap_npc_icon_exporter"
local lines = {}

local function log_line(text)
    pcall(function()
        local line = "[MinimapNpcIconExporter] " .. tostring(text)
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
    if value == nil then
        return nil
    end
    local ok, result = pcall(function() return value[key] end)
    if ok then
        return result
    end
    return nil
end

local function text(v)
    local ok, s = pcall(function() return tostring(v) end)
    if ok then
        return s
    end
    return "?"
end

local exported = {}

local function export_texture(resource, label)
    local data = field(resource, "data")
    local size = field(data, "size")
    local content = safe_call("get_content " .. label, function()
        return game.assets.get_content(field(data, "data"))
    end)
    local raw = content ~= nil and safe_call("read_data " .. label, function()
        return content:read_data()
    end) or nil
    if raw == nil then
        return nil
    end
    local file = PREFIX .. "/" .. label .. ".raw"
    local ok = safe_call("export " .. file, function()
        io.export(file, raw)
        return true
    end)
    if not ok then
        return nil
    end
    return file .. ":" .. text(field(size, "x")) .. "x" .. text(field(size, "y")) ..
        ":" .. text(field(data, "format")) .. ":" .. text(field(data, "levelCount")) ..
        ":" .. text(field(data, "debugName"))
end

local function export_image(image_guid, label)
    if image_guid == nil or image_guid == "" then
        return ""
    end
    local key = text(image_guid)
    if exported[key] ~= nil then
        return exported[key]
    end
    local parts = safe_call("get_resource_parts " .. key, function()
        return game.assets.get_resource_parts(image_guid, "keen::UiTextureResource")
    end)
    if parts == nil or #parts == 0 then
        local single = safe_call("get_resource " .. key, function()
            return game.assets.get_resource(image_guid, "keen::UiTextureResource")
        end)
        parts = single ~= nil and { single } or {}
    end
    local entries = {}
    for index, resource in ipairs(parts) do
        local entry = export_texture(resource, label .. "_p" .. tostring(index - 1))
        if entry ~= nil then
            entries[#entries + 1] = entry
        end
    end
    local joined = table.concat(entries, ";")
    exported[key] = joined
    return joined
end

log_line("version 0.1.0")

-- 1. UiMapResource: the map screen's own icon slots.
local ICON_FIELDS = { "iconNpc", "iconNpcBed", "iconAnimal", "iconFlameAltar", "iconPlayer", "iconOtherPlayer", "iconPing" }
local maps = safe_call("get_resources_by_type UiMapResource", function()
    return game.assets.get_resources_by_type("keen::UiMapResource")
end) or {}
log_line("UiMapResource count: " .. tostring(#maps))
local manifest = {}
for m, map in ipairs(maps) do
    local data = field(map, "data")
    for _, name in ipairs(ICON_FIELDS) do
        local slot = field(data, name)
        if slot ~= nil then
            local image = field(slot, "image")
            if image == nil then
                image = slot
            end
            local result = export_image(image, "map" .. tostring(m) .. "_" .. name)
            manifest[#manifest + 1] = name .. "\t" .. result
            log_line(name .. " -> " .. result)
        end
    end
    -- Dump every string-ish field name we can reach, to learn the layout.
    pcall(function()
        for key, value in pairs(data) do
            log_line("map" .. tostring(m) .. " field " .. text(key) .. " = " .. text(value))
        end
    end)
end

-- 2. Every UiTextureResource name, and the textures whose name suggests an NPC/person icon.
local textures = safe_call("get_resources_by_type UiTextureResource", function()
    return game.assets.get_resources_by_type("keen::UiTextureResource")
end) or {}
log_line("UiTextureResource count: " .. tostring(#textures))
local names = {}
local exportedByName = 0
for t, texture in ipairs(textures) do
    local data = field(texture, "data")
    local name = text(field(data, "debugName"))
    local size = field(data, "size")
    names[#names + 1] = name .. "\t" .. text(field(size, "x")) .. "x" .. text(field(size, "y")) .. "\t" .. text(field(texture, "guid"))
    local WANTED = { friend = true, journal_bases_npcs = true, player_position = true, hud_marker_outline = true,
        mapmarker_hint = true, compass_player = true, mapmarker_waypoint = true, custommarker_waypoint = true,
        mapmarker_completed = true, mapmarker_selector = true, hud_marker_fill = true, marker_ripple = true }
    if exportedByName < 40 and WANTED[name] then
        local entry = export_texture(texture, "tex" .. tostring(t) .. "_" .. string.gsub(name, "[^%w_]", "_"))
        if entry ~= nil then
            exportedByName = exportedByName + 1
            manifest[#manifest + 1] = name .. "\t" .. entry
        end
    end
end
pcall(function()
    io.export(PREFIX .. "/texture-names.tsv", table.concat(names, "\n"))
end)
pcall(function()
    io.export(PREFIX .. "/manifest.tsv", table.concat(manifest, "\n"))
end)
pcall(function()
    io.export(PREFIX .. "/export-log.txt", table.concat(lines, "\n"))
end)
log_line("done")

return {}
