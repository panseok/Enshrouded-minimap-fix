local MAP_GUID = "01bcbd07-bdbf-41c0-9999-5d58fb1a3aa1"
local EXPORT_PREFIX = "minimap_map_exporter"
local EXPORTER_VERSION = "0.1.8-gradient-luts"

local lines = {}

local function log_line(text)
    -- Wrapped in pcall on purpose: print()/logging here goes through a Rust
    -- boundary that requires valid UTF-8, and at least one of our own
    -- messages has already proven that's not guaranteed (embedding a
    -- tostring() of a game Buffer value produced invalid UTF-8 and crashed
    -- the entire mod loader, not just this mod). A logging call must never
    -- be able to take down the whole export.
    local ok = pcall(function()
        local line = "[MinimapMapExporter] " .. tostring(text)
        print(line)
        lines[#lines + 1] = line
    end)
    if not ok then
        pcall(function()
            local fallback = "[MinimapMapExporter] <a log message could not be printed -- likely contained invalid UTF-8>"
            print(fallback)
            lines[#lines + 1] = fallback
        end)
    end
end

log_line("version " .. EXPORTER_VERSION)

local function safe_call(label, fn)
    local ok, result = pcall(fn)
    if not ok then
        log_line(label .. " failed: " .. tostring(result))
        return nil
    end
    return result
end

local function describe_value(value)
    local value_type = type(value)
    if value_type == "string" or value_type == "number" or value_type == "boolean" then
        return value_type .. "=" .. tostring(value)
    end
    return value_type .. "=" .. tostring(value)
end

local function dump_fields(label, value)
    log_line(label .. ": " .. tostring(value))
    local ok, err = pcall(function()
        for key, field_value in pairs(value) do
            log_line("  " .. tostring(key) .. " -> " .. describe_value(field_value))
        end
    end)
    if not ok then
        log_line("  field dump unavailable: " .. tostring(err))
    end
end

local function looks_like_texture(data)
    return data ~= nil and
        (data.width ~= nil or data.size ~= nil) and
        data.format ~= nil and
        data.data ~= nil
end

local function texture_width(texture_data)
    if texture_data.width ~= nil then
        return texture_data.width
    end
    if texture_data.size ~= nil and texture_data.size.x ~= nil then
        return texture_data.size.x
    end
    return nil
end

local function texture_height(texture_data)
    if texture_data.height ~= nil then
        return texture_data.height
    end
    if texture_data.size ~= nil and texture_data.size.y ~= nil then
        return texture_data.size.y
    end
    return nil
end

local function content_hash_text(hash)
    if hash == nil then
        return ""
    end
    if type(hash) ~= "table" then
        return tostring(hash)
    end

    local parts = {}
    for _, key in ipairs({ "guid", "hash0", "hash1", "hash2" }) do
        if hash[key] ~= nil then
            parts[#parts + 1] = key .. "=" .. tostring(hash[key])
        end
    end
    if #parts > 0 then
        return table.concat(parts, ",")
    end
    return tostring(hash)
end

local function tsv_escape(value)
    local text = tostring(value or "")
    text = text:gsub("\t", " "):gsub("\r", " "):gsub("\n", " ")
    return text
end

local function texture_catalog_row(index, resource, content)
    local data = resource.data or {}
    local width = texture_width(data)
    local height = texture_height(data)

    return table.concat({
        tsv_escape(index),
        tsv_escape(resource.guid),
        tsv_escape(resource.type),
        tsv_escape(resource.part),
        tsv_escape(data.debugName),
        tsv_escape(width),
        tsv_escape(height),
        tsv_escape(data.type),
        tsv_escape(data.format),
        tsv_escape(data.levelCount),
        tsv_escape(data.isTiled),
        tsv_escape(content and content.size or ""),
        tsv_escape(content and content.hash0 or ""),
        tsv_escape(content and content.hash1 or ""),
        tsv_escape(content and content.hash2 or ""),
        tsv_escape(content_hash_text(data.data)),
    }, "\t")
end

local function looks_like_map_asset(resource)
    local data = resource.data
    if not looks_like_texture(data) then
        return false
    end

    local width = texture_width(data) or 0
    local height = texture_height(data) or 0
    if width < 256 or height < 256 or height <= 1 then
        return false
    end

    local text = string.lower(table.concat({
        tostring(resource.guid),
        tostring(resource.type),
        tostring(resource.part),
        tostring(data.debugName),
        tostring(data.type),
        tostring(data.format),
    }, " "))

    local needles = {
        "map",
        "minimap",
        "world",
        "terrain",
        "height",
        "baseheight",
        "detailheight",
        "region",
        "biome",
        "zone",
        "isoline",
        "sat",
        "satellite",
        "land",
        "ground",
        "atlas",
        "paper",
        "parch",
        "embervale",
        "ui_map",
    }

    for _, needle in ipairs(needles) do
        if string.find(text, needle, 1, true) ~= nil then
            return true
        end
    end

    return false
end

-- The raw texture bytes coming back from content:read_data() are a custom
-- "Buffer" userdata, not a plain Lua string -- it does not implement the
-- `#` length operator (confirmed by a hard crash: "attempt to get length of
-- a Buffer value"), and we don't know its exact method surface. Every probe
-- below is wrapped in pcall for that reason: NOTHING in this file is allowed
-- to throw an uncaught error, because that kills the entire mod loader for
-- every mod in the run, not just this one.
local warned_no_introspection = false

local function safe_buffer_len(bytes_str)
    local ok, result = pcall(function() return #bytes_str end)
    if ok and type(result) == "number" then
        return result
    end

    ok, result = pcall(function() return bytes_str:len() end)
    if ok and type(result) == "number" then
        return result
    end

    ok, result = pcall(function() return bytes_str.len end)
    if ok and type(result) == "number" then
        return result
    end

    return nil
end

local function safe_buffer_byte(bytes_str, offset)
    local ok, result = pcall(function() return string.byte(bytes_str, offset) end)
    if ok and type(result) == "number" then
        return result
    end

    ok, result = pcall(function() return bytes_str:byte(offset) end)
    if ok and type(result) == "number" then
        return result
    end

    ok, result = pcall(function() return bytes_str:get(offset - 1) end)
    if ok and type(result) == "number" then
        return result
    end

    ok, result = pcall(function() return bytes_str[offset - 1] end)
    if ok and type(result) == "number" then
        return result
    end

    ok, result = pcall(function() return bytes_str[offset] end)
    if ok and type(result) == "number" then
        return result
    end

    return nil
end

-- Samples a handful of evenly spaced bytes and returns true if they are all
-- identical (or the buffer is empty/unreadable). This catches the "decoded
-- to a single solid color" failure mode we hit with detail-height style
-- resources: the export succeeds structurally (right byte count, valid
-- PNG/RGBA container) but the underlying texture data is placeholder or
-- unsupported-format content that decoded to all zeros. If we can't safely
-- introspect the value at all, we log that once and report "not blank" --
-- i.e. this diagnostic quietly disables itself rather than ever risking a
-- crash of the whole export.
local function sample_is_blank(bytes_str, sample_count)
    if bytes_str == nil then
        return true
    end

    local length = safe_buffer_len(bytes_str)
    if length == nil then
        if not warned_no_introspection then
            warned_no_introspection = true
            log_line("NOTE blank-data detection is disabled: could not determine the length of the " ..
                "buffer value with any known method (#, :len(), .len). " ..
                "Exports will proceed normally; this diagnostic just won't catch blank candidates.")
        end
        return false
    end
    if length == 0 then
        return true
    end

    sample_count = sample_count or 256
    local first = nil
    local varies = false
    local sawAnyByte = false

    for i = 1, sample_count do
        local offset = 1 + math.floor((i - 1) * (length - 1) / math.max(sample_count - 1, 1))
        local byte = safe_buffer_byte(bytes_str, offset)
        if byte == nil then
            if not warned_no_introspection then
                warned_no_introspection = true
                log_line("NOTE blank-data detection is disabled: could not read individual bytes from the " ..
                    "buffer value with any known method (string.byte, :byte(), :get(), " ..
                    "[i]). Exports will proceed normally; this diagnostic just won't catch blank candidates.")
            end
            return false
        end
        sawAnyByte = true
        if first == nil then
            first = byte
        elseif byte ~= first then
            varies = true
            break
        end
    end

    if not sawAnyByte then
        return false
    end

    return not varies
end

local function export_texture(label, texture_data, options)
    if not looks_like_texture(texture_data) then
        return false
    end

    local width = texture_width(texture_data)
    local height = texture_height(texture_data)
    if width == nil or height == nil then
        return false
    end

    options = options or {}
    if options.skip_small_2d and (width < 64 or height < 64) then
        log_line(label .. " skipped small texture: " .. tostring(width) .. "x" .. tostring(height))
        return false
    end

    if options.only_2d and height <= 1 then
        log_line(label .. " skipped non-map texture: " .. tostring(width) .. "x" .. tostring(height))
        return false
    end

    log_line(label .. " texture candidate: " ..
        tostring(width) .. "x" .. tostring(height) ..
        " format=" .. tostring(texture_data.format))

    local content = safe_call(label .. " get_content", function()
        return game.assets.get_content(texture_data.data)
    end)
    if content == nil then
        return false
    end

    local raw = safe_call(label .. " read_data", function()
        return content:read_data()
    end)
    if raw == nil then
        return false
    end

    local base = EXPORT_PREFIX .. "/" .. label
    local rawLength = safe_buffer_len(raw)

    if sample_is_blank(raw, 256) then
        log_line("WARNING " .. label .. " raw content read from the game is a single repeated byte" ..
            " (length=" .. tostring(rawLength) .. ", format=" .. tostring(texture_data.format) ..
            "). This texture is likely an unloaded/placeholder mip or LOD, not real map data." ..
            " The exported PNG/RGBA for this candidate will probably be blank; try a different" ..
            " catalog candidate or texture index instead of stitching/converting this one.")
    end

    -- Always keep the untouched, pre-decode compressed/native bytes alongside
    -- the decoded PNG/RGBA. image.decode_texture does not reliably handle
    -- every pixel format here (BC4_unorm_block height data and R16_unorm
    -- height data in particular have come back visibly wrong -- a reddish,
    -- misinterpreted image instead of a clean grayscale height map). Having
    -- the raw bytes plus the exact format/width/height lets us decode those
    -- formats correctly offline instead of trusting this call blindly.
    io.export(base .. ".raw", raw)
    local rawMeta = table.concat({
        "label=" .. label,
        "format=" .. tostring(texture_data.format),
        "width=" .. tostring(width),
        "height=" .. tostring(height),
        "levelCount=" .. tostring(texture_data.levelCount),
        "byteLength=" .. tostring(rawLength),
        "debugName=" .. tostring(texture_data.debugName),
    }, "\n")
    io.export(base .. ".raw.meta.txt", rawMeta)
    log_line("exported " .. base .. ".raw (" .. tostring(rawLength) .. " bytes, format=" ..
        tostring(texture_data.format) .. ") with sidecar " .. base .. ".raw.meta.txt")

    local decoded = safe_call(label .. " decode_texture", function()
        return image.decode_texture(raw, texture_data.format, width, height)
    end)
    if decoded == nil then
        return false
    end

    local png = safe_call(label .. " encode png", function()
        return image.encode(decoded, "png")
    end)
    if png == nil then
        return false
    end

    io.export(base .. ".png", png)
    log_line("exported " .. base .. ".png")

    local rgba = safe_call(label .. " encode RGBA texture", function()
        return image.encode_texture(decoded, "R8G8B8A8_unorm")
    end)
    if rgba ~= nil then
        io.export(base .. ".rgba", rgba)
        log_line("exported " .. base .. ".rgba")

        if sample_is_blank(rgba, 256) then
            log_line("WARNING " .. label .. " decoded RGBA output is a single solid color" ..
                " (format=" .. tostring(texture_data.format) .. ", size=" ..
                tostring(width) .. "x" .. tostring(height) ..
                "). image.decode_texture likely does not support this pixel format and silently" ..
                " zero-filled the output, or the source texture itself has no data loaded." ..
                " Do not bother stitching/converting " .. base .. ".rgba into the minimap map" ..
                " asset -- pick a different candidate from the texture catalog instead.")
        end
    end

    return true
end

local function try_export_texture_resource_part(part_index, expected_tile_count)
    local resource = safe_call("get_resource keen::UiTextureResource part " .. tostring(part_index), function()
        return game.assets.get_resource(MAP_GUID, "keen::UiTextureResource", part_index)
    end)
    if resource == nil then
        return false
    end

    local data = resource.data
    dump_fields("keen::UiTextureResource part " .. tostring(part_index) .. " data", data)

    local label = "ui_map_part_" .. string.format("%03d", part_index)

    -- The map shader's 1D lookup-table textures (baseGradient, isolineGradient,
    -- fogZoneBorderGradient) are tiny (a few hundred pixels wide, 1 pixel tall)
    -- and were being silently dropped by the only_2d/skip_small_2d filters meant
    -- for icon-sized junk. These LUTs are exactly the color ramps the game's own
    -- map shader uses to turn elevation into pixel color and to draw isolines --
    -- decoding OUR composite with these exact ramps (instead of a hand-guessed
    -- palette) is how we match the real in-game/map-UI look. Always export them
    -- in full regardless of size.
    local is_gradient_lut = false
    pcall(function()
        local name = string.lower(tostring(data.debugName or ""))
        if string.find(name, "gradient", 1, true) ~= nil then
            is_gradient_lut = true
        end
    end)

    local export_options = { only_2d = true, skip_small_2d = true }
    if is_gradient_lut then
        export_options = {}
    end

    local exported = export_texture(label, data, export_options)
    if exported and expected_tile_count ~= nil then
        log_line("part " .. tostring(part_index) .. " exported; expected map tiles=" .. tostring(expected_tile_count))
    end
    return exported
end

local function try_export_texture_resource_parts(map_data)
    local expected_tile_count = nil
    if map_data ~= nil and map_data.tileCount ~= nil and map_data.tileCount.x ~= nil and map_data.tileCount.y ~= nil then
        expected_tile_count = map_data.tileCount.x * map_data.tileCount.y
    end

    local exported = false
    for part_index = 0, 210 do
        if try_export_texture_resource_part(part_index, expected_tile_count) then
            exported = true
        end
    end
    return exported
end

local function try_exact_resource(resource_type)
    local resource = safe_call("get_resource " .. resource_type, function()
        return game.assets.get_resource(MAP_GUID, resource_type, 0)
    end)
    if resource == nil then
        return false
    end

    log_line("found " .. resource_type .. " " .. MAP_GUID)
    dump_fields(resource_type .. " resource", resource)
    dump_fields(resource_type .. " data", resource.data)

    if export_texture("embervale_map_" .. resource_type:gsub("[^%w_]+", "_"), resource.data) then
        return true
    end

    if resource_type == "keen::UiMapResource" and try_export_texture_resource_parts(resource.data) then
        return true
    end

    local exported = false
    pcall(function()
        for key, field_value in pairs(resource.data) do
            if export_texture("embervale_map_field_" .. tostring(key), field_value) then
                exported = true
            end
        end
    end)

    return exported
end

local function scan_texture_resources()
    local resources = safe_call("get_resources_by_type keen::UiTextureResource", function()
        return game.assets.get_resources_by_type("keen::UiTextureResource")
    end)
    if resources == nil then
        return false
    end

    log_line("scanning " .. tostring(#resources) .. " UiTextureResource entries")
    local exported = false
    for index, resource in ipairs(resources) do
        local data = resource.data
        local text = string.lower(tostring(resource) .. " " .. tostring(data))
        local is_map_candidate =
            string.find(text, "map", 1, true) ~= nil or
            string.find(text, "embervale", 1, true) ~= nil or
            string.find(text, MAP_GUID, 1, true) ~= nil

        if is_map_candidate and looks_like_texture(data) then
            local label = "ui_texture_candidate_" .. tostring(index)
            if export_texture(label, data, { only_2d = true, skip_small_2d = true }) then
                exported = true
            end
        end
    end

    return exported
end

local function catalog_texture_resources()
    local resources = safe_call("get_resources_by_type keen::UiTextureResource for catalog", function()
        return game.assets.get_resources_by_type("keen::UiTextureResource")
    end)
    if resources == nil then
        return false
    end

    local rows = {
        table.concat({
            "index",
            "guid",
            "type",
            "part",
            "debugName",
            "width",
            "height",
            "textureType",
            "format",
            "levelCount",
            "isTiled",
            "contentSize",
            "contentHash0",
            "contentHash1",
            "contentHash2",
            "dataHash",
        }, "\t")
    }

    local exported = false
    local candidate_count = 0
    for index, resource in ipairs(resources) do
        local content = nil
        if resource.data ~= nil and resource.data.data ~= nil then
            content = safe_call("catalog content " .. tostring(index), function()
                return game.assets.get_content(resource.data.data)
            end)
        end

        rows[#rows + 1] = texture_catalog_row(index, resource, content)

        if looks_like_map_asset(resource) and candidate_count < 60 then
            candidate_count = candidate_count + 1
            log_line("catalog map-like candidate #" .. tostring(candidate_count) ..
                " index=" .. tostring(index) ..
                " guid=" .. tostring(resource.guid) ..
                " part=" .. tostring(resource.part) ..
                " name=" .. tostring(resource.data.debugName) ..
                " size=" .. tostring(texture_width(resource.data)) .. "x" .. tostring(texture_height(resource.data)) ..
                " format=" .. tostring(resource.data.format))

            local label = "catalog_candidate_" .. string.format("%03d", candidate_count) ..
                "_idx_" .. tostring(index) ..
                "_part_" .. tostring(resource.part)
            if export_texture(label, resource.data, { only_2d = true, skip_small_2d = true }) then
                exported = true
            end
        end
    end

    io.export(EXPORT_PREFIX .. "/texture-catalog.tsv", table.concat(rows, "\n"))
    log_line("texture catalog exported with " .. tostring(#resources) .. " UiTextureResource rows and " .. tostring(candidate_count) .. " map-like candidates")
    return exported
end

local exported =
    try_exact_resource("keen::UiMapResource") or
    try_exact_resource("keen::UiTextureResource") or
    scan_texture_resources()

local catalog_exported = catalog_texture_resources()
exported = exported or catalog_exported

if not exported then
    log_line("no map texture was exported; check this game's generated .cache/lua/types.lua for UiMapResource fields")
end

io.export(EXPORT_PREFIX .. "/export-log.txt", table.concat(lines, "\n"))

return {}
