-- One-shot exporter for the map marker icons used by the in-game world map.
-- For every keen::MapMarkerType in every keen::MapMarkerRegistryResource it writes
-- the icon (and muted icon) texture bytes at full resolution plus a manifest line.
-- tools/map-render/build_icon_atlas.py turns the export into embervale_minimap_icons.bin.
-- Every game call is wrapped in pcall: an uncaught error here takes down the whole
-- mod loader, not just this mod.
local PREFIX = "minimap_icon_exporter"
local lines = {}
local manifest = {}

local function log_line(text)
    pcall(function()
        local line = "[MinimapIconExporter] " .. tostring(text)
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

local function hash_value(h)
    local v = field(h, "value")
    if v ~= nil then
        return v
    end
    return h
end

local function text(v)
    local ok, s = pcall(function() return tostring(v) end)
    if ok then
        return s
    end
    return "?"
end

local exported_guids = {}

-- Exports every part of the UiTextureResource behind an image guid (the base level
-- and, for some icons, extra parts). Returns a "file:WxH:format" list.
local function export_image(image_guid, label)
    if image_guid == nil or image_guid == "" then
        return ""
    end
    local key = text(image_guid)
    if exported_guids[key] ~= nil then
        return exported_guids[key]
    end

    local parts = safe_call("get_resource_parts " .. key, function()
        return game.assets.get_resource_parts(image_guid, "keen::UiTextureResource")
    end)
    if parts == nil or #parts == 0 then
        local single = safe_call("get_resource " .. key, function()
            return game.assets.get_resource(image_guid, "keen::UiTextureResource")
        end)
        if single ~= nil then
            parts = { single }
        else
            parts = {}
        end
    end

    local entries = {}
    for index, resource in ipairs(parts) do
        local data = field(resource, "data")
        local size = field(data, "size")
        local content = safe_call("get_content " .. key, function()
            return game.assets.get_content(field(data, "data"))
        end)
        local raw = content ~= nil and safe_call("read_data " .. key, function()
            return content:read_data()
        end) or nil
        if raw ~= nil then
            local file = PREFIX .. "/" .. label .. "_p" .. tostring(index - 1) .. ".raw"
            local ok = safe_call("export " .. file, function()
                io.export(file, raw)
                return true
            end)
            if ok then
                entries[#entries + 1] = file .. ":" .. text(field(size, "x")) .. "x" .. text(field(size, "y")) ..
                    ":" .. text(field(data, "format")) .. ":" .. text(field(data, "levelCount")) ..
                    ":" .. text(field(data, "debugName"))
            end
        end
    end

    local joined = table.concat(entries, ";")
    exported_guids[key] = joined
    return joined
end

log_line("version 0.1.0")

local registries = safe_call("get_resources_by_type", function()
    return game.assets.get_resources_by_type("keen::MapMarkerRegistryResource")
end) or {}
log_line("registries: " .. tostring(#registries))

local count = 0
for r, registry in ipairs(registries) do
    local markers = field(field(registry, "data"), "mapMarkers")
    -- Array userdata: try the length operator, fall back to pairs().
    local list = {}
    local ok_len = pcall(function()
        for i = 1, #markers do
            list[#list + 1] = markers[i]
        end
    end)
    if not ok_len or #list == 0 then
        list = {}
        pcall(function()
            for _, m in pairs(markers) do
                list[#list + 1] = m
            end
        end)
    end
    log_line("registry " .. tostring(r) .. " guid=" .. text(field(registry, "guid")) .. " markers=" .. tostring(#list))
    for _, marker in ipairs(list) do
        if marker ~= nil then
            local id = hash_value(field(marker, "markerId"))
            local idnum = tonumber(id) or 0
            local label = string.format("m%08x", idnum)
            local icon = export_image(field(field(marker, "icon"), "image"), label .. "_icon")
            local muted = export_image(field(field(marker, "mutedIcon"), "image"), label .. "_muted")
            manifest[#manifest + 1] = table.concat({
                string.format("0x%08x", idnum),
                text(field(marker, "sortingCategory")),
                text(field(marker, "iconDisplaySize")),
                text(field(marker, "showAboveFogOfWar")),
                text(field(marker, "isFastTravelDestination")),
                text(field(marker, "minimizedColor")),
                text(field(marker, "typeName")),
                icon,
                muted,
            }, "\t")
            count = count + 1
        end
    end
end

log_line("exported " .. tostring(count) .. " marker types")
pcall(function()
    io.export(PREFIX .. "/manifest.tsv",
        "markerId\tsorting\tdisplaySize\taboveFog\tfastTravel\tminimizedColor\ttypeName\ticon\tmuted\n" ..
        table.concat(manifest, "\n"))
end)
pcall(function()
    io.export(PREFIX .. "/export-log.txt", table.concat(lines, "\n"))
end)

return {}
