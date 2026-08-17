--[[
Copyright © 2026, Nalfey of Asura
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of GEO-HUD nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Nalfey BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
]]

_addon.name = 'GEO-HUD'
_addon.author = 'Nalfey'
_addon.version = '1.9.1'
_addon.commands = {'geohud', 'gh'}

require('tables')
require('sets')
require('strings')
require('pack')
local bit = require('bit')
local config = require('config')
local files = require('files')
local xml = require('xml')
local texts = require('texts')
local images = require('images')
local packets = require('packets')
local res = require('resources')

-- Packed so the main chunk stays under Lua's 200-local limit.
local C = {
    geo_job = 21,
    luopan_ja = S{345, 353, 354, 355}, -- Full Circle, Concentric Pulse, Mending Halation, Radial Arcana
    widened = 508,
    entrust = 584,
    colure = 612,
    bolster = 513,
    radius = 6,
    -- Action categories that can generate enmity when they finish.
    tags = S{1, 2, 3, 4, 5, 6, 13, 14, 15},
}

local defaults = {
    text = {
        font = 'Arial',
        size = 11,
        alpha = 255,
        red = 240,
        green = 255,
        blue = 255,
        stroke = {width = 2, alpha = 255, red = 0, green = 38, blue = 62},
    },
    pos = {x = 180, y = 420},
    bg = {alpha = 150, red = 0, green = 0, blue = 0, visible = false},
    flags = {draggable = true, bold = false, italic = false},
    padding = 10,
    radius = 6,
    cure_enmity = true,
    ipc = true,
    always_show = false,
    show_hud = true,
    max_list = 8,
    show_mobs = false,
    scan_yalms = 35,
    rings = true,
    cardinal_chant = false,
    colorblind = false,
    max_rings = 50,
    orbs = true,
    orb_size = 72,
    orb_pad = 8,
    camera = {
        back = 6,
        height = 2.4,
        fov = 46,
        squash = 0.36,
        reverse = false,
        height_axis = 'z',
    },
}

local settings = config.load(defaults)
if settings.colorblind == nil then
    settings.colorblind = false
end
if settings.cardinal_chant == nil then
    settings.cardinal_chant = false
end
if type(settings.camera) ~= 'table' then
    settings.camera = {}
end
for key, value in pairs(defaults.camera) do
    if settings.camera[key] == nil then
        settings.camera[key] = value
    end
end
settings.text.font = 'Arial'
settings.text.size = 11
settings.text.red = 240
settings.text.green = 255
settings.text.blue = 255
settings.text.alpha = 255
settings.text.stroke.width = 2
settings.text.stroke.alpha = 255
settings.text.stroke.red = 0
settings.text.stroke.green = 38
settings.text.stroke.blue = 62

-- Per-character and per-job overlays live in data/<Name>.xml and data/<Name>_<JOB>.xml.
-- Toggles such as HUD/rings/orbs save to the current job file so GEO and other jobs
-- can show different pieces. Position and the rest stay in data/settings.xml.
-- Kept as one table so the main chunk stays under Lua's 200-local limit.
local profile = {
    keys = { 'show_hud', 'always_show', 'orbs', 'rings', 'cardinal_chant', 'show_mobs', 'max_list' },
    base = {},
    char = nil,
    job = nil,
}

function profile.snapshot(source)
    local snap = {}
    for i = 1, #profile.keys do
        local key = profile.keys[i]
        snap[key] = source[key]
    end
    return snap
end

function profile.apply_snap(target, snap)
    if not snap then return end
    for i = 1, #profile.keys do
        local key = profile.keys[i]
        if snap[key] ~= nil then
            target[key] = snap[key]
        end
    end
end

function profile.who()
    local player = windower.ffxi.get_player()
    if not player or type(player.name) ~= 'string' or player.name == '' then
        return nil, nil
    end
    local char = player.name:gsub('[^%w]', '')
    local job = player.main_job
    if type(job) == 'string' and job ~= '' then
        job = job:upper():gsub('[^%w]', '')
    else
        job = nil
    end
    if char == '' then
        return nil, nil
    end
    return char, job
end

function profile.path(char, job)
    if job and job ~= '' then
        return ('data/%s_%s.xml'):format(char, job)
    end
    return ('data/%s.xml'):format(char)
end

function profile.read(path)
    local file = files.new(path)
    if not file:exists() then
        return nil
    end
    local parsed = xml.read(file)
    if not parsed or parsed.type ~= 'tag' then
        return nil
    end
    local overlay = {}
    local children = parsed.children
    if not children then
        return overlay
    end
    local function parse_value(raw)
        if raw == nil then
            return nil
        end
        local text = tostring(raw):gsub('^%s+', ''):gsub('%s+$', '')
        if text:lower() == 'true' then
            return true
        end
        if text:lower() == 'false' then
            return false
        end
        return tonumber(text) or text
    end
    local function take(child)
        if not child or child.type ~= 'tag' then
            return
        end
        local key = tostring(child.name or ''):lower()
        local raw
        if child.children and child.children[1] and child.children[1].type == 'text' then
            raw = child.children[1].value
        else
            raw = child.value
        end
        overlay[key] = parse_value(raw)
    end
    if children.it then
        for child in children:it() do
            take(child)
        end
    else
        for i = 1, #children do
            take(children[i])
        end
    end
    return overlay
end

function profile.write(path, snap)
    local lines = {
        '<?xml version="1.1" ?>',
        '<settings>',
    }
    for i = 1, #profile.keys do
        local key = profile.keys[i]
        if snap[key] ~= nil then
            lines[#lines + 1] = ('    <%s>%s</%s>'):format(key, tostring(snap[key]), key)
        end
    end
    lines[#lines + 1] = '</settings>'
    lines[#lines + 1] = ''
    files.new(path):write(table.concat(lines, '\n'))
end

function profile.capture()
    profile.base = profile.snapshot(settings)
end

function profile.apply()
    profile.apply_snap(settings, profile.base)
    local char, job = profile.who()
    profile.char = char
    profile.job = job
    if not char then
        return
    end
    profile.apply_snap(settings, profile.read(profile.path(char)))
    if job then
        profile.apply_snap(settings, profile.read(profile.path(char, job)))
    end
    local player = windower.ffxi.get_player()
    if not player or (player.main_job_id ~= C.geo_job and player.main_job ~= 'GEO') then
        settings.cardinal_chant = false
    end
end

function profile.save()
    local current = profile.snapshot(settings)
    local char, job = profile.who()
    if char then
        profile.char = char
        profile.job = job
        if job then
            profile.write(profile.path(char, job), current)
        else
            profile.write(profile.path(char), current)
        end
        profile.apply_snap(settings, profile.base)
        config.save(settings)
        profile.apply_snap(settings, current)
    else
        config.save(settings)
    end
end

profile.capture()
profile.apply()

local hud = texts.new(settings)
hud:bg_visible(false)

local hud_panel = images.new({
    pos = {x = 0, y = 0},
    size = {width = 1, height = 1},
    color = {alpha = 150, red = 0, green = 0, blue = 0},
    draggable = false,
    visible = false,
})

-- Official Geo-/Indi- colures (BG-Wiki / FFXI wiki). Classified by effect, not
-- by the spell's targeting flags — those mark who you can cast on, not the aura.
local COLURE_BUFF = {
    Regen = true, Refresh = true, Haste = true,
    STR = true, DEX = true, VIT = true, AGI = true, INT = true, MND = true, CHR = true,
    Fury = true, Barrier = true, Acumen = true, Fend = true,
    Precision = true, Voidance = true, Focus = true, Attunement = true,
}
local COLURE_DEBUFF = {
    Poison = true, Wilt = true, Frailty = true, Fade = true, Malaise = true,
    Slip = true, Torpor = true, Vex = true, Languor = true, Slow = true,
    Paralysis = true, Gravity = true,
}

local spells = {
    geo = {},
    geo_el = {},
    geo_el_name = {},
    geo_buff_name = {},
    indi = {},
    indi_el = {},
    indi_st = {},
    indi_by_st = {},
    cure = S{},
    cura = S{},
    curaga = S{},
}

local tagged = {}          -- [mob_id] = os.clock()
local party_hate = {}      -- [ally_id] = { [mob_id] = true }  mobs that already have that ally on hate
local missing_since = {}   -- [mob_id] = os.clock() when the entity last disappeared
local apparently_dead = {} -- [mob_id] = os.clock() of first hpp==0 / dead status
local GONE_GRACE = 8
local DEAD_CONFIRM = 0.6
local last_spell = nil
local last_spell_element = nil
local last_spell_is_buff = false
local last_indi = nil
local last_indi_element = nil
local last_indi_status = nil
local last_indi_cast = 0
local MAX_ENTRUST = 5
local entrusted = {} -- {id, name, spell, element, cast}
local party_colure = {} -- [player_id] = true if Colure Active from 0x076
local party_widened = {} -- [player_id] = true if Widened Compass from 0x076
local party_slot_ids = {} -- [player_id] = true if present in last 0x076
local saw_party_buffs = false
local last_geo_id = nil
local last_luopan_id = nil
local bubble_hpp = 0
local bubble_alive = false
local hidden = false
local ring_test_until = 0
local debug_on = false
local last_hud = ''
local last_scan = 0
local last_ipc = 0
local last_ipc_payload = ''
local nearby_ids = {}      -- scanned enemy ids (positions are re-read each frame)
local ring_inside = {}     -- hysteresis: last in-bubble state per mob id
local ui = {x = 1920, y = 1080}
local logged_in = false
local RING_GREEN = 0xE63CFF6A
local RING_RED = 0xE6FF3C3C
local MAX_RINGS = 50
-- Cardinal Chant: target north/east/south/west of you. See bg-wiki Cardinal_Chant.
local chant = {
    north = 0xE63C8CFF,
    east = 0xE6FF3C3C,
    south = 0xE63CFF6A,
    west = 0xE6FFE03C,
    radius = 1.15,
    names = { 'Magic Crit', 'Magic Attack', 'Magic Accuracy', 'Magic Burst' },
    town_ids = S{
        26, 50, 53, 70, 80, 87, 94,
        230, 231, 232, 233, 234, 235, 236, 237,
        238, 239, 240, 241, 242, 243, 244, 245, 246,
        247, 248, 249, 250, 252, 256, 257,
        280, 281, 283, 284, 285,
    },
}

function chant.main_geo()
    local player = windower.ffxi.get_player()
    return player and (player.main_job_id == C.geo_job or player.main_job == 'GEO')
end

function chant.in_town()
    local info = windower.ffxi.get_info()
    if not info then
        return true
    end
    if info.mog_house then
        return true
    end
    return chant.town_ids[tonumber(info.zone) or 0] == true
end

function chant.enabled()
    return settings.cardinal_chant and chant.main_geo() and not chant.in_town()
end

local addon_dir = windower.addon_path:gsub('\\', '/'):gsub('/+$', '') .. '/'
package.cpath = addon_dir .. 'libs/?.dll;' .. package.cpath

local function open_ring_dll(filename, init)
    local path = addon_dir .. 'libs/' .. filename
    local opener, err = package.loadlib(path, init or 'luaopen_geohud_rings')
    if not opener then
        return false, tostring(err or (filename .. ' could not be loaded')) .. ' [' .. path .. ']'
    end
    local ok, result = pcall(opener)
    if not ok then
        return false, tostring(result) .. ' [' .. path .. ']'
    end
    if _GEORings == nil or _GEORings.native == nil then
        return false, filename .. ' loaded an old native module [' .. path .. ']'
    end
    return true, filename
end

local function require_ring_dll(module_name)
    local ok, err = pcall(require, module_name)
    if not ok then
        return false, tostring(err)
    end
    if _GEORings == nil or _GEORings.native == nil then
        return false, 'require ' .. module_name .. ' loaded an old native module'
    end
    return true, 'require ' .. module_name
end

local rings_loaded_from = nil
local rings_ok, rings_load_error = open_ring_dll('_GEORings25.dll', 'luaopen__GEORings25')
if rings_ok then
    rings_loaded_from = '_GEORings25.dll'
else
    local retry_ok, retry_from = require_ring_dll('_GEORings25')
    if retry_ok then
        rings_ok = true
        rings_loaded_from = retry_from
        rings_load_error = nil
    end
end

local function rings_available()
    return rings_ok and _GEORings ~= nil and _GEORings.native ~= nil
end

local function ring_cap()
    local cap = tonumber(settings.max_rings) or MAX_RINGS
    if cap < 1 then
        cap = 1
    elseif cap > MAX_RINGS then
        cap = MAX_RINGS
    end
    return cap
end

local function snapshot_ring_targets(luopan, radius)
    local wanted = {}
    local wanted_count = 0
    for i = 1, #nearby_ids do
        local id = nearby_ids[i]
        if not wanted[id] then
            wanted[id] = true
            wanted_count = wanted_count + 1
        end
    end
    for id in pairs(tagged) do
        if not wanted[id] then
            wanted[id] = true
            wanted_count = wanted_count + 1
        end
    end

    local live = {}
    if wanted_count == 0 then
        return live
    end

    local array = windower.ffxi.get_mob_array()
    if array then
        for _, mob in pairs(array) do
            if mob and mob.id and wanted[mob.id] then
                live[mob.id] = mob
                wanted[mob.id] = nil
            end
        end
    end

    for id in pairs(wanted) do
        live[id] = windower.ffxi.get_mob_by_id(id)
    end

    return live
end

-- FFXI element ids → asset folders (matches in-game Geo- spell orbs).
local ELEMENT_FOLDER = {
    [0] = 'Fire',
    [1] = 'Ice',
    [2] = 'Wind',
    [3] = 'Earth',
    [4] = 'Thunder',
    [5] = 'Water',
    [6] = 'Light',
    [7] = 'Dark',
}

local ORB_PERIOD = 0.12
local ORB_GLOW_PERIOD = 2.4
local ELEMENT_RGB = {
    Fire = {255, 110, 45},
    Ice = {150, 215, 255},
    Wind = {110, 255, 140},
    Earth = {230, 190, 80},
    Thunder = {210, 130, 255},
    Water = {70, 150, 255},
    Light = {255, 252, 210},
    Dark = {190, 100, 255},
}

local orb_glow = images.new({
    pos = {x = 0, y = 0},
    visible = false,
    draggable = false,
    size = {width = 140, height = 140},
    color = {alpha = 180, red = 255, green = 252, blue = 210},
    texture = {
        path = windower.addon_path .. 'assets/bubbles/orb_glow.png',
        fit = false,
    },
})
local orb_glow_visible = false

local orb = images.new({
    pos = {x = 0, y = 0},
    visible = false,
    draggable = false,
    size = {width = 72, height = 72},
    color = {alpha = 255, red = 255, green = 255, blue = 255},
    texture = {
        path = windower.addon_path .. 'assets/bubbles/Light/Orbs_Light_01.png',
        fit = false,
    },
})
local orb_visible = false
local orb_folder = nil
local orb_frame = 1
local orb_path = ''
local last_orb_tick = 0

local indi_orb = images.new({
    pos = {x = 0, y = 0},
    visible = false,
    draggable = false,
    size = {width = 36, height = 36},
    color = {alpha = 255, red = 255, green = 255, blue = 255},
    texture = {
        path = windower.addon_path .. 'assets/bubbles/Light/Orbs_Light_01.png',
        fit = false,
    },
})
local indi_orb_visible = false
local indi_orb_folder = nil
local indi_orb_frame = 1
local indi_orb_path = ''
local last_indi_orb_tick = 0

local entrust_orbs = {}
local entrust_orb_state = {}
for i = 1, MAX_ENTRUST do
    entrust_orbs[i] = images.new({
        pos = {x = 0, y = 0},
        visible = false,
        draggable = false,
        size = {width = 36, height = 36},
        color = {alpha = 255, red = 255, green = 255, blue = 255},
        texture = {
            path = windower.addon_path .. 'assets/bubbles/Light/Orbs_Light_01.png',
            fit = false,
        },
    })
    entrust_orb_state[i] = {visible = false, folder = nil, frame = 1, path = '', tick = 0}
end

local hpbar = {
    current = 0,
    target = 0,
    visible = false,
    w = 100,
    h = 8,
    fg_h = 3,
    inset_x = 4,
    inset_y = 3,
    anim = 0.1,
    r = 255,
    g = 149,
    b = 151,
    glow_a = 48,
    path = windower.addon_path .. 'assets/bars/',
}
local hpbar_bg = images.new({
    texture = {path = hpbar.path .. 'BarBG.png', fit = true},
    pos = {x = 0, y = 0},
    size = {width = hpbar.w, height = hpbar.h},
    color = {alpha = 255, red = 255, green = 255, blue = 255},
    draggable = false,
    visible = false,
})
local hpbar_fg = images.new({
    pos = {x = 0, y = 0},
    size = {width = 0, height = hpbar.fg_h},
    color = {alpha = 255, red = hpbar.r, green = hpbar.g, blue = hpbar.b},
    draggable = false,
    visible = false,
})
local hpbar_glow_sides = images.new({
    texture = {path = hpbar.path .. 'BarGlowSides.png', fit = true},
    pos = {x = 0, y = 0},
    size = {width = 6, height = 32},
    color = {alpha = hpbar.glow_a, red = hpbar.r, green = hpbar.g, blue = hpbar.b},
    draggable = false,
    visible = false,
})
local hpbar_glow_mid = images.new({
    texture = {path = hpbar.path .. 'BarGlowMid.png', fit = true},
    pos = {x = 0, y = 0},
    size = {width = 0, height = 32},
    color = {alpha = hpbar.glow_a, red = hpbar.r, green = hpbar.g, blue = hpbar.b},
    draggable = false,
    visible = false,
})
local hp_pct = texts.new({
    pos = {x = 0, y = 0},
    text = {
        font = 'Arial',
        size = 11,
        alpha = 255,
        red = 240,
        green = 255,
        blue = 255,
        stroke = {width = 2, alpha = 255, red = 0, green = 38, blue = 62},
    },
    bg = {visible = false, alpha = 0},
    flags = {draggable = false, bold = false, italic = false},
    padding = 0,
})
hp_pct:hide()
local hp_pct_visible = false
local range_label = texts.new({
    pos = {x = 0, y = 0},
    text = {
        font = 'Arial',
        size = 11,
        alpha = 255,
        red = 240,
        green = 255,
        blue = 255,
        stroke = {width = 2, alpha = 255, red = 0, green = 38, blue = 62},
    },
    bg = {visible = false, alpha = 0},
    flags = {draggable = false, bold = false, italic = false},
    padding = 0,
})
range_label:hide()
local range_label_visible = false

-- ── helpers ────────────────────────────────────────────────────────────────

local function chat(message)
    windower.add_to_chat(207, 'GEO-HUD: ' .. message)
end

local function cs(r, g, b, s)
    return ('\\cs(%d,%d,%d)%s\\cr'):format(r, g, b, s)
end

local function refresh_ui()
    local w = windower.get_windower_settings()
    if w then
        ui.x = w.ui_x_res or w.x_res or 1920
        ui.y = w.ui_y_res or w.y_res or 1080
    end
end

local function is_geo_job(player)
    if not player then return false end
    return player.main_job_id == C.geo_job or player.sub_job_id == C.geo_job
        or player.main_job == 'GEO' or player.sub_job == 'GEO'
end

local function has_buff(id)
    local player = windower.ffxi.get_player()
    if not player or not player.buffs then return false end
    for i = 1, #player.buffs do
        if player.buffs[i] == id then return true end
    end
    return false
end

local function current_radius()
    local radius = tonumber(settings.radius) or C.radius
    if has_buff(C.widened) then
        radius = radius * 2
    elseif last_geo_id and party_widened[last_geo_id] then
        radius = radius * 2
    end
    return radius
end

local function is_luopan(mob)
    if not mob or not mob.name then return false end
    return mob.name:lower():find('luopan', 1, true) ~= nil
end

local function height_of(mob)
    if not mob then return 0 end
    if settings.camera.height_axis == 'y' then
        return tonumber(mob.y) or 0
    end
    return tonumber(mob.z) or 0
end

local function ground_xy(mob)
    if not mob then return 0, 0 end
    if settings.camera.height_axis == 'y' then
        return tonumber(mob.x) or 0, tonumber(mob.z) or 0
    end
    return tonumber(mob.x) or 0, tonumber(mob.y) or 0
end

local function dist3(a, b)
    if not a or not b then return 9999 end
    local ax, ay = ground_xy(a)
    local bx, by = ground_xy(b)
    local dz = height_of(a) - height_of(b)
    local dx, dy = ax - bx, ay - by
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

local function is_dead_mob(mob)
    if not mob then return false end
    local hpp = tonumber(mob.hpp)
    if hpp ~= nil and hpp <= 0 then return true end
    local status = tonumber(mob.status) or 0
    return status == 2 or status == 3
end

local function is_enemy(mob)
    if not mob then return false end
    if not mob.is_npc then return false end
    if mob.in_party or mob.in_alliance or mob.charmed then return false end
    if is_luopan(mob) then return false end
    if is_dead_mob(mob) then return false end
    -- valid_target flickers on some updates; don't require it for already-known enemies
    local spawn = tonumber(mob.spawn_type) or 0
    if spawn == 16 then return true end
    if mob.claim_id and mob.claim_id ~= 0 then return true end
    return false
end

-- Shown on the HUD / rings. Stale entity slots (despawned Umbrils, etc.) stay
-- in get_mob_array() with leftover coords but valid_target is false.
local function is_listed_enemy(mob)
    if not is_enemy(mob) then return false end
    if not mob.valid_target then return false end
    if (tonumber(mob.id) or 0) == 0 then return false end
    if (tonumber(mob.index) or 0) == 0 then return false end
    if not mob.name or mob.name == '' then return false end
    if (tonumber(mob.spawn_type) or 0) ~= 16 then return false end
    local x, y = ground_xy(mob)
    if x == 0 and y == 0 then return false end
    return true
end

local function party_mobs()
    local out = {}
    local party = windower.ffxi.get_party()
    if not party then return out end
    local slots = {'p0', 'p1', 'p2', 'p3', 'p4', 'p5', 'a10', 'a11', 'a12', 'a13', 'a14', 'a15', 'a20', 'a21', 'a22', 'a23', 'a24', 'a25'}
    for i = 1, #slots do
        local member = party[slots[i]]
        if member and member.mob and member.mob.id then
            out[member.mob.id] = member.mob
        end
    end
    return out
end

-- Geo- buffs apply to the Geomancer's party (p0-p5), including Trusts.
local function party_member_mobs()
    local out = {}
    local party = windower.ffxi.get_party()
    if not party then return out end
    for i = 0, 5 do
        local member = party['p' .. i]
        local mob = member and member.mob
        if mob and mob.id and (tonumber(mob.index) or 0) ~= 0 then
            out[#out + 1] = mob
        end
    end
    return out
end

local function colure_effect(name)
    if type(name) ~= 'string' then
        return nil
    end
    local suffix = name:match('^[Gg]eo%-(.+)$') or name:match('^[Ii]ndi%-(.+)$')
    if not suffix then
        return nil
    end
    suffix = suffix:gsub('%s+', '')
    if COLURE_BUFF[suffix] then
        return true
    end
    if COLURE_DEBUFF[suffix] then
        return false
    end
    return nil
end

local function remember_geo_spell(spell_id, spell_name)
    if spell_name then
        last_spell = spell_name
        last_spell_element = spells.geo_el_name[spell_name] or spells.geo_el[spell_id]
        local is_buff = colure_effect(spell_name)
        if is_buff == nil and spells.geo_buff_name[spell_name] ~= nil then
            is_buff = spells.geo_buff_name[spell_name]
        end
        last_spell_is_buff = is_buff == true
        return
    end
    last_spell_is_buff = false
end

local function bubble_is_buff()
    local named = last_spell and colure_effect(last_spell)
    if named ~= nil then
        return named
    end
    if last_spell and spells.geo_buff_name[last_spell] ~= nil then
        return spells.geo_buff_name[last_spell]
    end
    return last_spell_is_buff
end

local function is_cure_spell_name(name)
    if type(name) ~= 'string' then return false end
    -- Cure / Cure II..VI, Cura / Cura II/III, Curaga / Curaga II..V, Full Cure.
    -- Does not match Cursna, Cura-na, etc.
    return name:sub(1, 4) == 'Cure' or name:sub(1, 4) == 'Cura' or name == 'Full Cure'
end

local function build_spells()
    spells.geo = {}
    spells.geo_el = {}
    spells.geo_el_name = {}
    spells.geo_buff_name = {}
    spells.indi = {}
    spells.indi_el = {}
    spells.indi_st = {}
    spells.indi_by_st = {}
    spells.cure = S{}
    spells.cura = S{}
    spells.curaga = S{}
    if not res or not res.spells then return end
    for id, spell in pairs(res.spells) do
        if spell.type == 'Geomancy' and type(spell.en) == 'string' then
            if spell.en:sub(1, 4) == 'Geo-' then
                spells.geo[id] = spell.en
                spells.geo_el[id] = spell.element
                spells.geo_el_name[spell.en] = spell.element
                spells.geo_buff_name[spell.en] = colure_effect(spell.en) == true
            elseif spell.en:sub(1, 5) == 'Indi-' then
                spells.indi[id] = spell.en
                spells.indi_el[id] = spell.element
                spells.indi_st[id] = spell.status
                if spell.status then
                    spells.indi_by_st[spell.status] = {
                        name = spell.en,
                        element = spell.element,
                    }
                end
            end
        elseif is_cure_spell_name(spell.en) then
            spells.cure:add(id)
            if spell.en:sub(1, 6) == 'Curaga' then
                spells.curaga:add(id)
            elseif spell.en == 'Cura' or spell.en:sub(1, 5) == 'Cura ' then
                spells.cura:add(id)
            end
        end
    end
end

local function note_self_indi(spell_id)
    if not spell_id or not spells.indi[spell_id] then return end
    last_indi = spells.indi[spell_id]
    last_indi_element = spells.indi_el[spell_id]
    last_indi_status = spells.indi_st[spell_id]
    last_indi_cast = os.clock()
    last_hud = ''
end

local function party_member_name(id)
    if not id then return nil end
    local party = windower.ffxi.get_party()
    if party then
        for i = 0, 5 do
            local p = party['p' .. i]
            if p then
                if p.mob and p.mob.id == id then return p.name end
            end
        end
    end
    local mob = windower.ffxi.get_mob_by_id(id)
    return mob and mob.name
end

local function ally_in_party(id)
    if not id then return false end
    if party_slot_ids[id] then return true end
    local party = windower.ffxi.get_party()
    if not party then return false end
    for i = 1, 5 do
        local p = party['p' .. i]
        if p and p.mob and p.mob.id == id then
            return true
        end
    end
    return false
end

local function note_entrust(spell_id, target_id)
    if not spell_id or not target_id or not spells.indi[spell_id] then return end
    local player = windower.ffxi.get_player()
    local me = windower.ffxi.get_mob_by_target('me')
    local my_id = (player and player.id) or (me and me.id)
    if my_id and target_id == my_id then return end

    local name = party_member_name(target_id) or '?'
    for i = 1, #entrusted do
        if entrusted[i].id == target_id then
            entrusted[i].name = name
            entrusted[i].spell = spells.indi[spell_id]
            entrusted[i].element = spells.indi_el[spell_id]
            entrusted[i].cast = os.clock()
            last_hud = ''
            return
        end
    end
    while #entrusted >= MAX_ENTRUST do
        table.remove(entrusted, 1)
    end
    entrusted[#entrusted + 1] = {
        id = target_id,
        name = name,
        spell = spells.indi[spell_id],
        element = spells.indi_el[spell_id],
        cast = os.clock(),
    }
    last_hud = ''
end

local function refresh_entrusts()
    if #entrusted == 0 then return end
    local now = os.clock()
    local kept = {}
    for i = 1, #entrusted do
        local e = entrusted[i]
        local pending = (now - (e.cast or 0)) < 2.5
        local still_up = pending or (not saw_party_buffs) or (party_slot_ids[e.id] and party_colure[e.id])
        if still_up then
            local name = party_member_name(e.id)
            if name then e.name = name end
            kept[#kept + 1] = e
        end
    end
    if #kept ~= #entrusted then
        last_hud = ''
    end
    entrusted = kept
end

local function watching_own_indi()
    return is_geo_job(windower.ffxi.get_player())
end

local function watched_geo_has_colure()
    if watching_own_indi() then
        return has_buff(C.colure)
    end
    return last_geo_id ~= nil and party_colure[last_geo_id] == true
end

-- Indi- and Geo- share the same buff IDs (Haste 580, Regen 539, …). The only
-- reliable "an Indi is on me" flag is Colure Active. Which Indi it is comes
-- from finished/started Indi- casts, not from those overlapping buffs.
local function refresh_indi()
    local pending = last_indi and (os.clock() - last_indi_cast) < 2.5
    if pending or watched_geo_has_colure() then
        return
    end
    if not watching_own_indi() and last_indi and not saw_party_buffs then
        return
    end
    last_indi = nil
    last_indi_element = nil
    last_indi_status = nil
end

local function actor_id_is_me(actor_id)
    if not actor_id then return false end
    local player = windower.ffxi.get_player()
    if player and player.id == actor_id then return true end
    local me = windower.ffxi.get_mob_by_target('me')
    return me and me.id == actor_id
end

local function action_target_id(act)
    local target = act and act.targets and act.targets[1]
    return target and target.id
end

-- Category 8 = start casting, 4 = magic lands. A different target is Entrust.
-- Own GEO: track our Indi / Entrusts. Party HUD: track the party GEO the same way.
local function note_indi_from_action(act)
    if not act or (act.category ~= 4 and act.category ~= 8) then
        return
    end
    if not spells.indi[act.param] then
        return
    end

    local player = windower.ffxi.get_player()
    local me = windower.ffxi.get_mob_by_target('me')
    local my_id = (player and player.id) or (me and me.id)
    local actor = act.actor_id
    if not actor then return end

    local ours = actor_id_is_me(actor)
    if not ours then
        if is_geo_job(player) then
            return
        end
        local allies = party_mobs()
        if not allies[actor] then
            return
        end
        if last_geo_id and actor ~= last_geo_id then
            return
        end
        last_geo_id = actor
    end

    local tid = action_target_id(act)
    if tid then
        if my_id and ours and tid ~= my_id then
            note_entrust(act.param, tid)
            return
        end
        if not ours and tid ~= actor then
            note_entrust(act.param, tid)
            return
        end
    elseif ours and has_buff(C.entrust) then
        return
    end
    note_self_indi(act.param)
end

-- ── luopan / GEO identity ──────────────────────────────────────────────────

local function find_self_luopan()
    local pet = windower.ffxi.get_mob_by_target('pet')
    if pet and is_luopan(pet) and (tonumber(pet.hpp) or 0) > 0 then
        return pet
    end
    local player = windower.ffxi.get_player()
    if player and player.mob and player.mob.pet_index and player.mob.pet_index ~= 0 then
        local by_index = windower.ffxi.get_mob_by_index(player.mob.pet_index)
        if by_index and is_luopan(by_index) and (tonumber(by_index.hpp) or 0) > 0 then
            return by_index
        end
    end
    return nil
end

local function find_party_luopan()
    local party = windower.ffxi.get_party()
    if not party then return nil, nil end
    local slots = {'p0', 'p1', 'p2', 'p3', 'p4', 'p5'}
    for i = 1, #slots do
        local member = party[slots[i]]
        if member and member.mob and member.mob.pet_index and member.mob.pet_index ~= 0 then
            local pet = windower.ffxi.get_mob_by_index(member.mob.pet_index)
            if pet and is_luopan(pet) and (tonumber(pet.hpp) or 0) > 0 then
                return pet, member.mob.id
            end
        end
    end
    return nil, nil
end

-- Returns luopan mob, geomancer entity id, and whether *we* are that GEO.
local function resolve_bubble()
    local player = windower.ffxi.get_player()
    local me = windower.ffxi.get_mob_by_target('me')
    local self_id = me and me.id or (player and player.id)

    local self_pet = find_self_luopan()
    if self_pet then
        last_geo_id = self_id
        last_luopan_id = self_pet.id
        return self_pet, self_id, true
    end

    local party_pet, owner_id = find_party_luopan()
    if party_pet then
        last_geo_id = owner_id
        last_luopan_id = party_pet.id
        return party_pet, owner_id, owner_id == self_id
    end

    if last_luopan_id then
        local leftover = windower.ffxi.get_mob_by_id(last_luopan_id)
        if leftover and is_luopan(leftover) and (tonumber(leftover.hpp) or 0) > 0 then
            return leftover, last_geo_id, last_geo_id == self_id
        end
    end

    if is_geo_job(player) then
        return nil, last_geo_id or self_id, true
    end
    return nil, last_geo_id, false
end

-- ── enmity tags ────────────────────────────────────────────────────────────

local function tag_mob(id)
    if not id or id == 0 then return end
    local mob = windower.ffxi.get_mob_by_id(id)
    if mob and (not mob.is_npc or is_luopan(mob) or mob.in_party or mob.in_alliance) then
        return
    end
    tagged[id] = os.clock()
    missing_since[id] = nil
end

local function untag_mob(id)
    tagged[id] = nil
    missing_since[id] = nil
    apparently_dead[id] = nil
    for _, mobs in pairs(party_hate) do
        mobs[id] = nil
    end
end

local function clear_tags()
    tagged = {}
    party_hate = {}
    missing_since = {}
    apparently_dead = {}
end

-- Hate sticks until the mob is actually dead or gone for several seconds.
-- Do not drop tags because of a missing lookup, valid_target flicker, or a
-- 0x00E packet that didn't include HP (those zero the HP byte).
local function prune_tags()
    local now = os.clock()
    for id in pairs(tagged) do
        local mob = windower.ffxi.get_mob_by_id(id)
        if not mob then
            apparently_dead[id] = nil
            if not missing_since[id] then
                missing_since[id] = now
            elseif now - missing_since[id] > GONE_GRACE then
                tagged[id] = nil
                missing_since[id] = nil
            end
        elseif is_dead_mob(mob) then
            -- A single 0-HP frame is common on entity updates; wait to confirm.
            if not apparently_dead[id] then
                apparently_dead[id] = now
            elseif now - apparently_dead[id] > DEAD_CONFIRM then
                tagged[id] = nil
                missing_since[id] = nil
                apparently_dead[id] = nil
            end
        else
            missing_since[id] = nil
            apparently_dead[id] = nil
        end
    end
    for ally_id, mobs in pairs(party_hate) do
        for mob_id in pairs(mobs) do
            local mob = windower.ffxi.get_mob_by_id(mob_id)
            if not mob then
                if not missing_since[mob_id] then
                    missing_since[mob_id] = now
                elseif now - missing_since[mob_id] > GONE_GRACE then
                    mobs[mob_id] = nil
                end
            elseif is_dead_mob(mob) then
                mobs[mob_id] = nil
            end
        end
        if not next(mobs) then
            party_hate[ally_id] = nil
        end
    end
end

local function note_hate(ally_id, mob_id)
    if not ally_id or not mob_id or ally_id == 0 or mob_id == 0 then return end
    local list = party_hate[ally_id]
    if not list then
        list = {}
        party_hate[ally_id] = list
    end
    list[mob_id] = true
end

local function tag_from_ally_hate(ally_id)
    local list = party_hate[ally_id]
    if not list then return end
    for mob_id in pairs(list) do
        tag_mob(mob_id)
    end
end

local function tag_allies_near(origin, radius)
    if not origin then return end
    local allies = party_mobs()
    for ally_id, ally_mob in pairs(allies) do
        if dist3(origin, ally_mob) <= radius then
            tag_from_ally_hate(ally_id)
        end
    end
end

-- Remember which mobs already have a given party member on their hate list
-- (mob hits them, or they hit the mob). Used so Cure only tags those mobs.
local function record_party_hate(act)
    if not act or not act.targets then return end
    local allies = party_mobs()
    local actor_id = act.actor_id
    if not actor_id then return end

    local actor_is_ally = allies[actor_id] ~= nil
    local actor_mob = windower.ffxi.get_mob_by_id(actor_id)
    local actor_is_enemy = actor_mob and is_enemy(actor_mob)

    for i = 1, act.target_count or #act.targets do
        local target = act.targets[i]
        local tid = target and target.id
        if tid then
            if actor_is_enemy and allies[tid] then
                note_hate(tid, actor_id)
            elseif actor_is_ally then
                local tmob = windower.ffxi.get_mob_by_id(tid)
                if tmob and is_enemy(tmob) then
                    note_hate(actor_id, tid)
                end
            end
        end
    end
end

local function actor_is_geo(actor_id, geo_id)
    return actor_id and geo_id and actor_id == geo_id
end

local function apply_action_tags(act, geo_id)
    if not act then return end

    record_party_hate(act)

    -- Anyone finishing a Geo- spell is the Geomancer we should watch.
    -- Geo- itself does not put surrounding enemies on the enmity list.
    local is_geo_spell = act.category == 4 and act.param and spells.geo[act.param]
    if is_geo_spell then
        remember_geo_spell(act.param, spells.geo[act.param])
        last_geo_id = act.actor_id
        geo_id = act.actor_id
    end

    note_indi_from_action(act)

    if not geo_id then return end
    if not C.tags[act.category] then return end

    local allies = party_mobs()

    if actor_is_geo(act.actor_id, geo_id) then
        if act.category == 6 and C.luopan_ja[act.param] then
            bubble_alive = false
            bubble_hpp = 0
            last_luopan_id = nil
        end

        if is_geo_spell then return end
        if not act.targets then return end

        local is_cure = act.category == 4 and act.param and spells.cure[act.param]
        local listed = {}
        for i = 1, act.target_count or #act.targets do
            local target = act.targets[i]
            if target and target.id then
                listed[target.id] = true
                local mob = windower.ffxi.get_mob_by_id(target.id)
                if mob and is_enemy(mob) then
                    -- Spell / melee / JA directly on the enemy → GEO is on that hate list.
                    tag_mob(target.id)
                elseif is_cure and settings.cure_enmity and allies[target.id] then
                    -- Cure a member who already has enmity vs a mob → GEO joins that mob's list.
                    tag_from_ally_hate(target.id)
                end
            end
        end

        -- Cura is self-centered AoE and may only list the caster as the target.
        if is_cure and settings.cure_enmity and spells.cura[act.param] then
            tag_allies_near(windower.ffxi.get_mob_by_id(geo_id), 10.5)
        end

        -- Curaga is centered on the selected ally. The action packet sometimes
        -- only names that primary target, so also tag every party member in
        -- the 10-yalm splash (each may be on a different mob).
        if is_cure and settings.cure_enmity and spells.curaga[act.param] then
            local hub = nil
            for id in pairs(listed) do
                if allies[id] then
                    hub = windower.ffxi.get_mob_by_id(id)
                    if hub then break end
                end
            end
            if not hub then
                hub = windower.ffxi.get_mob_by_id(geo_id)
            end
            tag_allies_near(hub, 10.5)
        end
        return
    end

    -- Enemy acting on the Geomancer generates enmity (same as the GEO hitting them).
    if not act.targets then return end
    local hit_geo = false
    for i = 1, act.target_count or #act.targets do
        local target = act.targets[i]
        if target and target.id == geo_id then
            hit_geo = true
            break
        end
    end
    if hit_geo then
        tag_mob(act.actor_id)
    end
end

-- ── HUD ────────────────────────────────────────────────────────────────────

local function hp_text_color(pct)
    if pct > 75 then return 240, 255, 255 end
    if pct > 50 then return 243, 243, 124 end
    if pct > 25 then return 248, 186, 128 end
    return 252, 129, 130
end

local function hide_hp_bar()
    if hp_pct_visible then
        hp_pct:hide()
        hp_pct_visible = false
    end
    if range_label_visible then
        range_label:hide()
        range_label_visible = false
    end
    if not hpbar.visible then return end
    hpbar_bg:hide()
    hpbar_fg:hide()
    hpbar_glow_mid:hide()
    hpbar_glow_sides:hide()
    hpbar.visible = false
end

local function hide_hud_panel()
    hud_panel:hide()
end

local function update_hud_panel()
    hud:bg_visible(false)
    hide_hud_panel()
end

local function show_hp_bar()
    if hpbar.visible then return end
    hpbar_bg:show()
    hpbar_fg:show()
    hpbar_glow_mid:show()
    hpbar_glow_sides:show()
    hpbar.visible = true
end

local function update_hp_bar(alive, pct)
    if hidden or settings.show_hud == false or not alive then
        hide_hp_bar()
        return
    end

    pct = math.max(0, math.min(100, tonumber(pct) or 0))
    hpbar.target = pct / 100

    local diff = hpbar.target - hpbar.current
    if math.abs(diff) > 0.001 then
        hpbar.current = hpbar.current + diff * hpbar.anim
    else
        hpbar.current = hpbar.target
    end

    local hx, hy = hud:pos()
    local pad = tonumber(settings.padding) or 10
    local bx = hx + pad
    local by = hy + pad + 22
    local fill_w = math.max(0, math.min(hpbar.w - hpbar.inset_x * 2, math.ceil(hpbar.w * hpbar.current)))

    hpbar_bg:pos(bx, by)
    hpbar_bg:size(hpbar.w, hpbar.h)
    hpbar_bg:color(255, 255, 255)
    hpbar_bg:alpha(255)
    hpbar_fg:pos(bx + hpbar.inset_x, by + hpbar.inset_y)
    hpbar_fg:size(fill_w, hpbar.fg_h)
    hpbar_fg:color(hpbar.r, hpbar.g, hpbar.b)
    hpbar_fg:alpha(255)

    local glow_y = by - (32 - 10) / 2
    hpbar_glow_sides:pos(bx - 3, glow_y)
    hpbar_glow_sides:size(6, 32)
    hpbar_glow_mid:pos(bx, glow_y)
    hpbar_glow_mid:size(math.max(fill_w, 1), 32)
    hpbar_glow_mid:color(hpbar.r, hpbar.g, hpbar.b)
    hpbar_glow_sides:color(hpbar.r, hpbar.g, hpbar.b)
    if math.abs(diff) > 0.01 then
        hpbar_glow_mid:alpha(hpbar.glow_a)
        hpbar_glow_sides:alpha(hpbar.glow_a)
    else
        hpbar_glow_mid:alpha(0)
        hpbar_glow_sides:alpha(0)
    end

    local r, g, b = hp_text_color(pct)
    local hp_str = ('%d%%'):format(math.floor(pct + 0.5))
    hp_pct:color(r, g, b)
    hp_pct:text(hp_str)
    local tw = select(1, hp_pct:extents()) or 0
    if tw < 8 or tw > 48 then
        tw = 7 * #hp_str
    end
    hp_pct:pos(bx + hpbar.w - tw, by + hpbar.h - 6)
    if not hp_pct_visible then
        hp_pct:show()
        hp_pct_visible = true
    end

    show_hp_bar()
end

local function scan_nearby(me, luopan)
    nearby_ids = {}
    if not me then return end
    local array = windower.ffxi.get_mob_array()
    if not array then return end
    local scan = tonumber(settings.scan_yalms) or 35
    local origin = luopan or me
    local found = {}
    local seen = {}
    for _, mob in pairs(array) do
        if mob and mob.id and is_listed_enemy(mob) then
            local d = dist3(origin, mob)
            if d <= scan then
                found[#found + 1] = mob
                seen[mob.id] = true
            end
        end
    end
    for id in pairs(tagged) do
        if not seen[id] then
            local mob = windower.ffxi.get_mob_by_id(id)
            if mob and is_listed_enemy(mob) and dist3(origin, mob) <= scan then
                found[#found + 1] = mob
            end
        end
    end
    table.sort(found, function(a, b)
        local ra = current_radius()
        local a_tag = tagged[a.id] and 1 or 0
        local b_tag = tagged[b.id] and 1 or 0
        local a_in = dist3(origin, a) <= ra and 1 or 0
        local b_in = dist3(origin, b) <= ra and 1 or 0
        -- Un-tagged in-range first (still need a hit), then tagged in-range, then the rest.
        local a_need = (a_in == 1 and a_tag == 0) and 0 or 1
        local b_need = (b_in == 1 and b_tag == 0) and 0 or 1
        if a_need ~= b_need then return a_need < b_need end
        if a_in ~= b_in then return a_in > b_in end
        if a_tag ~= b_tag then return a_tag > b_tag end
        -- Stable order so same-named mobs don't swap lines every refresh.
        if a.id ~= b.id then return a.id < b.id end
        return dist3(origin, a) < dist3(origin, b)
    end)
    for i = 1, #found do
        nearby_ids[i] = found[i].id
    end
end

local function hide_orb()
    if orb_glow_visible then
        orb_glow:hide()
        orb_glow_visible = false
    end
    if orb_visible then
        orb:hide()
        orb_visible = false
    end
    orb_folder = nil
end

local function hide_indi_orb()
    if indi_orb_visible then
        indi_orb:hide()
        indi_orb_visible = false
    end
    indi_orb_folder = nil
end

local function hide_entrust_orbs()
    for i = 1, MAX_ENTRUST do
        local st = entrust_orb_state[i]
        if st.visible then
            entrust_orbs[i]:hide()
            st.visible = false
        end
        st.folder = nil
    end
end

local function orb_slot()
    local hx, hy = hud:pos()
    local size = tonumber(settings.orb_size) or 72
    local pad = tonumber(settings.orb_pad) or 8
    local ox = hx - size - pad
    if ox < 0 then
        ox = 0
    end
    return ox, hy, size
end

local function update_orb(alive)
    if hidden or settings.show_hud == false or not alive or settings.orbs == false then
        hide_orb()
        return
    end

    if last_spell_element == nil and last_spell then
        last_spell_element = spells.geo_el_name[last_spell]
    end

    local folder = ELEMENT_FOLDER[last_spell_element] or 'Light'
    local now = os.clock()
    if folder ~= orb_folder then
        orb_folder = folder
        orb_frame = 1
        last_orb_tick = now
    elseif now - last_orb_tick >= ORB_PERIOD then
        last_orb_tick = now
        orb_frame = orb_frame % 5 + 1
    end

    local ox, hy, size = orb_slot()
    -- Stacked luopan+indi: nudge both orbs up so they sit on the text.
    if last_indi or watched_geo_has_colure() then
        hy = hy - 5
    end
    orb:pos(ox, hy)
    orb:size(size, size)

    local path = windower.addon_path .. ('assets/bubbles/%s/Orbs_%s_%02d.png'):format(folder, folder, orb_frame)
    if path ~= orb_path then
        orb_path = path
        orb:path(path)
        orb:size(size, size)
    end

    local rgb = ELEMENT_RGB[folder] or ELEMENT_RGB.Light
    local pulse = 0.5 + 0.5 * math.sin(now * (2 * math.pi / ORB_GLOW_PERIOD))
    -- Primitive is a square; the PNG is a circle with empty margin so the
    -- quad edges never show. Scale up so the visible bloom matches 1.85-2.05x.
    local glow_scale = 2.78 + 0.70 * pulse
    local glow_alpha = 130 + 110 * pulse
    if has_buff(C.bolster) then
        glow_scale = glow_scale * 2
        glow_alpha = math.min(255, glow_alpha * 2)
    end
    local glow_size = math.floor(size * glow_scale + 0.5)
    local gx = ox - math.floor((glow_size - size) * 0.5)
    local gy = hy - math.floor((glow_size - size) * 0.5)
    orb_glow:pos(gx, gy)
    orb_glow:size(glow_size, glow_size)
    orb_glow:color(rgb[1], rgb[2], rgb[3])
    orb_glow:alpha(math.floor(glow_alpha + 0.5))
    if not orb_glow_visible then
        orb_glow:show()
        orb_glow_visible = true
    end
    if not orb_visible then
        orb:show()
        orb_visible = true
    end
end

local function update_indi_orb(active)
    if hidden or settings.show_hud == false or not active or settings.orbs == false then
        hide_indi_orb()
        return
    end

    local folder = ELEMENT_FOLDER[last_indi_element] or 'Light'
    local now = os.clock()
    if folder ~= indi_orb_folder then
        indi_orb_folder = folder
        indi_orb_frame = 1
        last_indi_orb_tick = now
    elseif now - last_indi_orb_tick >= ORB_PERIOD then
        last_indi_orb_tick = now
        indi_orb_frame = indi_orb_frame % 5 + 1
    end

    local ox, hy, size = orb_slot()
    local small = math.max(16, math.floor(size * 0.5 + 0.5))
    local hx = hud:pos()
    local pad = tonumber(settings.padding) or 10
    local orb_pad = tonumber(settings.orb_pad) or 8
    local y_nudge = math.floor((small - 16) / 2)
    local ix, iy
    if orb_visible then
        ix = ox + size - small + 6
        -- INDI is two blanks after Range (line 5) when a luopan is up.
        iy = hy + pad + 5 * 16 - y_nudge + 5
    else
        ix = hx - small - orb_pad
        if ix < 0 then
            ix = 0
        end
        iy = hy + pad + 2 * 16 - y_nudge + 5
    end
    indi_orb:pos(ix, iy)
    indi_orb:size(small, small)

    local path = windower.addon_path .. ('assets/bubbles/%s/Orbs_%s_%02d.png'):format(folder, folder, indi_orb_frame)
    if path ~= indi_orb_path then
        indi_orb_path = path
        indi_orb:path(path)
        indi_orb:size(small, small)
    end
    if not indi_orb_visible then
        indi_orb:show()
        indi_orb_visible = true
    end
end

local function update_entrust_orbs()
    if hidden or settings.show_hud == false or #entrusted == 0 or settings.orbs == false then
        hide_entrust_orbs()
        return
    end

    local ox, _, size = orb_slot()
    local small = math.max(16, math.floor(size * 0.5 + 0.5))
    local hx, hy = hud:pos()
    local pad = tonumber(settings.padding) or 10
    local orb_pad = tonumber(settings.orb_pad) or 8
    local ix
    if orb_visible then
        ix = ox + size - small + 6
    else
        ix = hx - small - orb_pad
        if ix < 0 then
            ix = 0
        end
    end
    local y_nudge = math.floor((small - 16) / 2)
    -- Two blanks after INDI (and after Range when a bubble is up) so 36px orbs don't overlap.
    local first_line = bubble_alive and 8 or 5
    local now = os.clock()

    for i = 1, MAX_ENTRUST do
        local img = entrust_orbs[i]
        local st = entrust_orb_state[i]
        local e = entrusted[i]
        if not e then
            if st.visible then
                img:hide()
                st.visible = false
            end
            st.folder = nil
        else
            local folder = ELEMENT_FOLDER[e.element] or 'Light'
            if folder ~= st.folder then
                st.folder = folder
                st.frame = 1
                st.tick = now
            elseif now - st.tick >= ORB_PERIOD then
                st.tick = now
                st.frame = st.frame % 5 + 1
            end
            local iy = hy + pad + (first_line + (i - 1) * 3) * 16 - y_nudge + 8
            img:pos(ix, iy)
            img:size(small, small)
            local path = windower.addon_path .. ('assets/bubbles/%s/Orbs_%s_%02d.png'):format(folder, folder, st.frame)
            if path ~= st.path then
                st.path = path
                img:path(path)
                img:size(small, small)
            end
            if not st.visible then
                img:show()
                st.visible = true
            end
        end
    end
end

local function update_hud(luopan, geo_id, we_are_geo)
    if hidden or settings.show_hud == false then
        hud:hide()
        hide_orb()
        hide_indi_orb()
        hide_entrust_orbs()
        hide_hp_bar()
        hide_hud_panel()
        return
    end

    local player = windower.ffxi.get_player()
    local show = settings.always_show or we_are_geo or is_geo_job(player) or luopan or last_spell or last_indi or last_geo_id or #entrusted > 0 or chant.enabled()
    if not show then
        hud:hide()
        hide_orb()
        hide_indi_orb()
        hide_entrust_orbs()
        hide_hp_bar()
        hide_hud_panel()
        last_hud = ''
        return
    end

    local radius = current_radius()
    bubble_alive = luopan ~= nil
    bubble_hpp = luopan and (tonumber(luopan.hpp) or 0) or 0

    local lines = {}

    if bubble_alive then
        lines[#lines + 1] = cs(120, 255, 140, 'LUOPAN') .. '  ' .. cs(240, 255, 255, last_spell or 'Geo- ???')
        lines[#lines + 1] = ''
        lines[#lines + 1] = ''
    else
        lines[#lines + 1] = cs(255, 90, 90, 'NO LUOPAN')
    end

    local in_range, tagged_in, tagged_total = 0, 0, 0
    local potency
    local potency_color = {200, 200, 200}
    if bubble_is_buff() then
        local party_total = 0
        for _, mob in ipairs(party_member_mobs()) do
            if not is_luopan(mob) and not is_dead_mob(mob) then
                party_total = party_total + 1
                if luopan and dist3(luopan, mob) <= radius + 0.15 then
                    in_range = in_range + 1
                end
            end
        end
        potency = (not bubble_alive and 'no bubble')
            or (party_total == 0 and 'no party in range')
            or ('%d/%d party in range'):format(in_range, party_total)
        if bubble_alive and party_total > 0 then
            if in_range == party_total then potency_color = {120, 255, 140}
            elseif in_range == 0 then potency_color = {255, 90, 90}
            else potency_color = {255, 210, 70} end
        end
    else
        for id in pairs(tagged) do
            tagged_total = tagged_total + 1
            local mob = windower.ffxi.get_mob_by_id(id)
            if mob and luopan and dist3(luopan, mob) <= radius + 0.15 then
                tagged_in = tagged_in + 1
            end
        end
        for i = 1, #nearby_ids do
            local mob = windower.ffxi.get_mob_by_id(nearby_ids[i])
            if mob and luopan and dist3(luopan, mob) <= radius + 0.15 then
                in_range = in_range + 1
            end
        end
        potency = (not bubble_alive and 'no bubble')
            or (in_range == 0 and 'no enemies in range')
            or ('%d/%d in range tagged'):format(tagged_in, in_range)
        if bubble_alive and in_range > 0 then
            if tagged_in == in_range then potency_color = {120, 255, 140}
            elseif tagged_in == 0 then potency_color = {255, 90, 90}
            else potency_color = {255, 210, 70} end
        end
    end

    local range_text = ('Range %.1f"   %s'):format(radius, cs(potency_color[1], potency_color[2], potency_color[3], potency))
    if bubble_alive then
        local hx, hy = hud:pos()
        local pad = tonumber(settings.padding) or 10
        local by = hy + pad + 22
        range_label:text(range_text)
        range_label:pos(hx + pad, by + hpbar.h + 9)
        if not range_label_visible then
            range_label:show()
            range_label_visible = true
        end
    else
        if range_label_visible then
            range_label:hide()
            range_label_visible = false
        end
    end
    lines[#lines + 1] = ''
    if bubble_alive then
        lines[#lines + 1] = ''
    end

    if last_indi then
        lines[#lines + 1] = cs(120, 255, 140, 'INDI') .. '  ' .. cs(240, 255, 255, last_indi)
    elseif watched_geo_has_colure() then
        lines[#lines + 1] = cs(120, 255, 140, 'INDI') .. '  ' .. cs(160, 160, 160, 'Indi- ???')
    else
        lines[#lines + 1] = cs(255, 90, 90, 'NO INDI')
    end
    if #entrusted > 0 then
        lines[#lines + 1] = ''
        lines[#lines + 1] = ''
    end
    for i = 1, #entrusted do
        local e = entrusted[i]
        local name = e.name or '?'
        if #name > 12 then name = name:sub(1, 11) .. '.' end
        local spell = e.spell or 'Indi- ???'
        lines[#lines + 1] = cs(120, 255, 140, 'ENTRUST') .. '  ' .. cs(240, 255, 255, spell .. '  ' .. name)
        if i < #entrusted then
            lines[#lines + 1] = ''
            lines[#lines + 1] = ''
        end
    end
    local chant_line = chant.hud_line()
    if chant_line then
        lines[#lines + 1] = ''
        lines[#lines + 1] = chant_line
    end

    local listed = 0
    if settings.show_mobs ~= false then
        listed = math.min(#nearby_ids, tonumber(settings.max_list) or 8)
    end
    if listed > 0 then
        lines[#lines + 1] = cs(140, 140, 140, '------------------------')
        for i = 1, listed do
            local mob = windower.ffxi.get_mob_by_id(nearby_ids[i])
            if mob then
                local d = luopan and dist3(luopan, mob) or dist3(windower.ffxi.get_mob_by_target('me'), mob)
                local is_tagged = tagged[mob.id] ~= nil
                local in_b = luopan and d <= radius + 0.15
                local mark, status, rgb
                if is_tagged and in_b then
                    mark, status, rgb = '+', 'ACTIVE', {120, 255, 140}
                elseif is_tagged then
                    mark, status, rgb = '~', 'OUT', {255, 210, 70}
                elseif in_b then
                    mark, status, rgb = '!', 'NO HATE', {255, 90, 90}
                else
                    mark, status, rgb = '-', 'far', {150, 150, 150}
                end
                local name = mob.name or '?'
                if #name > 16 then name = name:sub(1, 15) .. '.' end
                lines[#lines + 1] = cs(rgb[1], rgb[2], rgb[3],
                    ('%s %-16s %4.1f  %s'):format(mark, name, d, status))
            end
        end
    end

    if debug_on then
        lines[#lines + 1] = cs(180, 180, 180, ('geo:%s tags:%d rings:%s'):format(
            tostring(geo_id or '-'), tagged_total, settings.rings and 'on' or 'off'))
    end

    local text = table.concat(lines, '\n')
    if text ~= last_hud then
        hud:text(text)
        last_hud = text
    end
    hud:show()
    update_hud_panel()
end

-- ── IPC (multibox / same-PC party) ─────────────────────────────────────────

local function split_pipe(msg)
    local parts = {}
    local start = 1
    while true do
        local s, e = msg:find('|', start, true)
        if not s then
            parts[#parts + 1] = msg:sub(start)
            break
        end
        parts[#parts + 1] = msg:sub(start, s - 1)
        start = e + 1
    end
    return parts
end

local function send_ipc(luopan, geo_id)
    if not settings.ipc then return end
    local player = windower.ffxi.get_player()
    local info = windower.ffxi.get_info()
    if not player or not info then return end

    local ids = {}
    for id in pairs(tagged) do
        ids[#ids + 1] = tostring(id)
    end
    table.sort(ids)
    local enc = {}
    for i = 1, #entrusted do
        local e = entrusted[i]
        local name = (e.name or '?'):gsub('[~;|]', '')
        enc[#enc + 1] = ('%s~%s~%s~%s'):format(tostring(e.id or 0), e.spell or '', tostring(e.element or ''), name)
    end
    local payload = table.concat({
        'GH',
        '2',
        player.name,
        tostring(info.zone or 0),
        tostring(geo_id or 0),
        tostring(luopan and luopan.id or 0),
        tostring(bubble_hpp or 0),
        tostring(last_spell or ''),
        ('%.1f'):format(current_radius()),
        table.concat(ids, ','),
        tostring(last_indi or ''),
        tostring(last_indi_element or ''),
        table.concat(enc, ';'),
    }, '|')

    local now = os.clock()
    if payload == last_ipc_payload and now - last_ipc < 1.0 then
        return
    end
    last_ipc = now
    last_ipc_payload = payload
    windower.send_ipc_message(payload)
end

local function on_ipc(msg)
    if not settings.ipc or type(msg) ~= 'string' then return end
    if msg:sub(1, 3) ~= 'GH|' then return end
    local parts = split_pipe(msg)
    if #parts < 9 then return end

    local player = windower.ffxi.get_player()
    if player and parts[3] == player.name then return end

    local info = windower.ffxi.get_info()
    if not info or tonumber(parts[4]) ~= info.zone then return end

    -- Merge tags from the GEO's client (covers cure-enmity the local client may miss).
    local ids = parts[10] or ''
    if ids ~= '' then
        for id in ids:gmatch('%d+') do
            tag_mob(tonumber(id))
        end
    end
    if parts[8] ~= '' then
        remember_geo_spell(nil, parts[8])
    end
    local gid = tonumber(parts[5])
    if gid and gid ~= 0 then
        if last_geo_id == nil or not watching_own_indi() then
            last_geo_id = gid
        end
    end
    if not watching_own_indi() then
        if parts[11] and parts[11] ~= '' then
            last_indi = parts[11]
            last_indi_element = tonumber(parts[12])
            last_indi_cast = os.clock()
            last_hud = ''
        end
        if parts[13] and parts[13] ~= '' then
            local rebuilt = {}
            for token in parts[13]:gmatch('[^;]+') do
                local id, spell, elem, name = token:match('^(%d+)~([^~]*)~([^~]*)~(.*)$')
                if id then
                    rebuilt[#rebuilt + 1] = {
                        id = tonumber(id),
                        spell = spell ~= '' and spell or 'Indi- ???',
                        element = tonumber(elem),
                        name = name ~= '' and name or '?',
                        cast = os.clock(),
                    }
                end
            end
            if #rebuilt > 0 then
                entrusted = rebuilt
                last_hud = ''
            end
        end
    end
end

-- ── lifecycle ──────────────────────────────────────────────────────────────

local function reset_zone()
    clear_tags()
    last_spell = nil
    last_spell_element = nil
    last_spell_is_buff = false
    last_indi = nil
    last_indi_element = nil
    last_indi_status = nil
    entrusted = {}
    last_geo_id = nil
    last_luopan_id = nil
    bubble_alive = false
    bubble_hpp = 0
    nearby_ids = {}
    last_hud = ''
    last_ipc_payload = ''
    hud:hide()
    hide_orb()
    hide_indi_orb()
    hide_entrust_orbs()
    hide_hp_bar()
    hide_hud_panel()
    if rings_available() and _GEORings.chant then
        _GEORings.chant(0, 0, 0, 0, 0, 0)
    end
end

local function estimate_ring_radius(mob)
    local humanoid = 1.15
    local player_foot = 0.64
    if not mob then
        return player_foot
    end
    if not mob.is_npc then
        return player_foot
    end

    local size = tonumber(mob.model_size) or 0
    local scale = tonumber(mob.model_scale) or 1
    if scale <= 0 then
        scale = 1
    end

    local extent
    if size > 0.05 then
        extent = size * scale
        if extent > humanoid then
            extent = humanoid + math.sqrt(extent - humanoid) * 0.62
        end
    else
        -- LuaCore often zeros model_size; invert scale around 0.5 so large
        -- models (small scale) get larger rings instead of tiny ones.
        extent = humanoid * (0.5 / math.max(scale, 0.18))
        if extent > 3.0 then
            extent = 3.0 + math.sqrt(extent - 3.0) * 0.5
        end
    end

    local footprint = extent * 0.5
    if footprint < 0.45 then
        footprint = 0.45
    elseif footprint > 8 then
        footprint = 8
    end
    return footprint
end

local function apply_colorblind()
    if rings_available() and _GEORings.colorblind then
        _GEORings.colorblind(settings.colorblind and 1 or 0)
    end
end

local function start_rings()
    if rings_available() then
        _GEORings.start()
        apply_colorblind()
    end
end

local function stop_rings()
    if rings_available() then
        if _GEORings.chant then
            _GEORings.chant(0, 0, 0, 0, 0, 0)
        end
        _GEORings.clear()
        if _GEORings.commit then
            _GEORings.commit()
        end
        _GEORings.stop()
    end
    ring_inside = {}
end

function chant.channel(color, shift)
    return math.floor(color / (2 ^ shift)) % 256
end

function chant.pack(r, g, b, a)
    return a * 0x1000000 + r * 0x10000 + g * 0x100 + b
end

function chant.mix(c0, c1, t)
    if t <= 0 then
        return c0
    end
    if t >= 1 then
        return c1
    end
    local r = chant.channel(c0, 16) + (chant.channel(c1, 16) - chant.channel(c0, 16)) * t
    local g = chant.channel(c0, 8) + (chant.channel(c1, 8) - chant.channel(c0, 8)) * t
    local b = chant.channel(c0, 0) + (chant.channel(c1, 0) - chant.channel(c0, 0)) * t
    local a = chant.channel(c0, 24) + (chant.channel(c1, 24) - chant.channel(c0, 24)) * t
    return chant.pack(math.floor(r + 0.5), math.floor(g + 0.5), math.floor(b + 0.5), math.floor(a + 0.5))
end

function chant.sector(east, north)
    local ang = math.atan2(east, north)
    if ang < 0 then
        ang = ang + math.pi * 2
    end
    local sector = ang / (math.pi * 0.5)
    local index = math.floor(sector)
    return index, sector - index
end

function chant.color_for(east, north)
    local index, t = chant.sector(east, north)
    local palette = {chant.north, chant.east, chant.south, chant.west, chant.north}
    return chant.mix(palette[index + 1], palette[index + 2], t)
end

function chant.parts(east, north)
    local index, t = chant.sector(east, north)
    local first = chant.names[index + 1]
    local second = chant.names[(index + 1) % 4 + 1]
    if t < 0.10 then
        return first
    end
    if t > 0.90 then
        return second
    end
    if t <= 0.5 then
        return first, second
    end
    return second, first
end

function chant.offset(me, tgt)
    if not me or not tgt then
        return nil
    end
    local px, py = ground_xy(me)
    local tx, ty = ground_xy(tgt)
    local east = tx - px
    local north = ty - py
    if (east * east + north * north) < 0.0225 then
        return nil
    end
    return east, north
end

function chant.hud_line()
    if not chant.enabled() then
        return nil
    end
    local me = windower.ffxi.get_mob_by_target('me')
    local tgt = chant.target_of(me)
    local east, north = chant.offset(me, tgt)
    if east == nil then
        return nil
    end
    local color = chant.color_for(east, north)
    local r = chant.channel(color, 16)
    local g = chant.channel(color, 8)
    local b = chant.channel(color, 0)
    local main, other = chant.parts(east, north)
    if not other then
        return cs(r, g, b, main)
    end
    return cs(r, g, b, main) .. cs(math.floor(r * 0.50 + 0.5), math.floor(g * 0.50 + 0.5), math.floor(b * 0.50 + 0.5), ' / ' .. other)
end

function chant.target_of(me)
    if not me then
        return nil
    end
    local names = {'t', 'bt'}
    for i = 1, #names do
        local tgt = windower.ffxi.get_mob_by_target(names[i])
        if tgt and tgt.id and tgt.id ~= me.id and (tonumber(tgt.index) or 0) ~= 0
            and not is_dead_mob(tgt) then
            return tgt
        end
    end
    return nil
end

function chant.submit()
    if not rings_available() or not _GEORings.chant then
        return
    end
    if hidden or not chant.enabled() then
        _GEORings.chant(0, 0, 0, 0, 0, 0)
        return
    end

    local me = windower.ffxi.get_mob_by_target('me')
    local tgt = chant.target_of(me)
    if not me or (tonumber(me.index) or 0) == 0 or not tgt then
        _GEORings.chant(0, 0, 0, 0, 0, 0)
        return
    end

    local east, north = chant.offset(me, tgt)
    if east == nil then
        _GEORings.chant(0, 0, 0, 0, 0, 0)
        return
    end

    start_rings()
    _GEORings.chant(
        tonumber(me.index) or 0,
        tonumber(me.x) or 0,
        tonumber(me.y) or 0,
        tonumber(me.z) or 0,
        chant.radius,
        chant.color_for(east, north))
end

function profile.apply_visuals()
    last_hud = ''
    apply_colorblind()
    if hidden or settings.show_hud == false then
        hud:hide()
        hide_orb()
        hide_indi_orb()
        hide_entrust_orbs()
        hide_hp_bar()
        hide_hud_panel()
    end
    if not hidden and (settings.rings or chant.enabled()) then
        start_rings()
        if not settings.rings and rings_available() then
            _GEORings.clear()
            if _GEORings.commit then
                _GEORings.commit()
            end
        end
    else
        stop_rings()
    end
    if rings_available() and _GEORings.chant and not chant.enabled() then
        _GEORings.chant(0, 0, 0, 0, 0, 0)
    end
end

function profile.flush()
    if profile.char then
        profile.write(profile.path(profile.char, profile.job), profile.snapshot(settings))
    end
    local current = profile.snapshot(settings)
    profile.apply_snap(settings, profile.base)
    config.save(settings)
    profile.apply_snap(settings, current)
end

config.register(settings, function()
    profile.capture()
    profile.apply()
    profile.apply_visuals()
end)

local function publish_rings()
    if rings_available() and _GEORings.commit then
        _GEORings.commit()
    end
end

-- valid_target and 0-HP bytes flicker on entity updates. Tagged / already-scanned
-- mobs stay on the ring list until prune_tags or scan_nearby drops them.
local function is_ring_mob(mob, known)
    if not mob or (tonumber(mob.index) or 0) == 0 then
        return false
    end
    if is_luopan(mob) or mob.in_party or mob.in_alliance then
        return false
    end
    if known then
        if tagged[mob.id] then
            return true
        end
        return not is_dead_mob(mob)
    end
    return is_listed_enemy(mob)
end

local function submit_rings(luopan)
    if not rings_available() then
        return
    end

    start_rings()
    apply_colorblind()
    _GEORings.clear()

    if hidden or not settings.rings then
        publish_rings()
        ring_inside = {}
        return
    end

    local function add_mob(mob, green)
        if not mob or (tonumber(mob.index) or 0) == 0 then
            return
        end
        _GEORings.add(
            tonumber(mob.index) or 0,
            tonumber(mob.x) or 0,
            tonumber(mob.y) or 0,
            tonumber(mob.z) or 0,
            estimate_ring_radius(mob),
            green and RING_GREEN or RING_RED)
    end

    local me = windower.ffxi.get_mob_by_target('me')
    if me and me.index and me.index ~= 0 then
        _GEORings.player(me.index, tonumber(me.x) or 0, tonumber(me.y) or 0, tonumber(me.z) or 0)
    else
        _GEORings.player(0, 0, 0, 0)
    end

    if ring_test_until > 0 and os.clock() < ring_test_until then
        add_mob(me, true)
        publish_rings()
        return
    end
    if ring_test_until > 0 and os.clock() >= ring_test_until then
        ring_test_until = 0
    end

    local cap = ring_cap()
    if luopan == nil then
        luopan = select(1, resolve_bubble())
    end
    local radius = current_radius()

    if bubble_is_buff() then
        local still_inside = {}
        local candidates = {}
        if luopan then
            for _, mob in ipairs(party_member_mobs()) do
                if not is_luopan(mob) and not is_dead_mob(mob) then
                    local dist = dist3(luopan, mob)
                    local was_inside = ring_inside[mob.id]
                    local in_bubble
                    if was_inside then
                        in_bubble = dist <= radius + 0.70
                    else
                        in_bubble = dist <= radius + 0.15
                    end
                    if in_bubble then
                        still_inside[mob.id] = true
                        candidates[#candidates + 1] = {id = mob.id, mob = mob, dist = dist}
                    end
                end
            end
            table.sort(candidates, function(a, b)
                if a.dist ~= b.dist then
                    return a.dist < b.dist
                end
                return a.id < b.id
            end)
            for i = 1, math.min(#candidates, cap) do
                add_mob(candidates[i].mob, true)
            end
        end
        ring_inside = still_inside
        publish_rings()
        return
    end

    local live = snapshot_ring_targets(luopan, radius)
    local candidates = {}
    local seen = {}
    local still_inside = {}

    local function consider(id, known)
        if seen[id] then
            return
        end
        local mob = live[id]
        if not mob or not is_ring_mob(mob, known) then
            return
        end
        local is_tagged = tagged[id] ~= nil
        local dist = luopan and dist3(luopan, mob) or 9999
        local was_inside = ring_inside[id]
        local in_bubble
        if not luopan then
            in_bubble = false
        elseif was_inside then
            in_bubble = dist <= radius + 0.70
        else
            in_bubble = dist <= radius + 0.15
        end
        if not is_tagged and not in_bubble then
            return
        end
        seen[id] = true
        still_inside[id] = in_bubble
        candidates[#candidates + 1] = {
            id = id,
            mob = mob,
            green = is_tagged and in_bubble,
            in_bubble = in_bubble and 1 or 0,
            dist = dist,
        }
    end

    for i = 1, #nearby_ids do
        consider(nearby_ids[i], true)
    end
    for id in pairs(tagged) do
        consider(id, true)
    end

    ring_inside = still_inside

    table.sort(candidates, function(a, b)
        if a.in_bubble ~= b.in_bubble then
            return a.in_bubble > b.in_bubble
        end
        if a.green ~= b.green then
            return a.green and not b.green
        end
        if a.dist ~= b.dist then
            return a.dist < b.dist
        end
        return a.id < b.id
    end)

    for i = 1, math.min(#candidates, cap) do
        local entry = candidates[i]
        add_mob(entry.mob, entry.green)
    end
    publish_rings()
end

local function report_rings()
    if rings_available() then
        chat('Ground rings: ' .. tostring(_GEORings.status()))
    else
        chat('Ground rings: native module failed to load.')
        chat(tostring(rings_load_error))
        chat('Expected addons/GEO-HUD/libs/_GEORings25.dll.')
    end
    chat('Rings drawing ' .. (settings.rings and 'on' or 'off') .. '  (//geohud rings on|off)')
end

local function print_help()
    chat('Commands:')
    chat('  //geohud show | hide | reset')
    chat('  //geohud hud on|off       — hide text/orbs, keep ground rings')
    chat('  //geohud radius <yalms>   — bubble radius (default 6; Widened Compass auto-doubles)')
    chat('  //geohud ipc on|off       — share tags/Indi/Entrust with other local Windower instances')
    chat('  //geohud orbs on|off      — luopan / Indi / Entrust bubble animations')
    chat('  //geohud rings on|off     — debuff: green = tagged in bubble; buff: green = party in bubble')
    chat('  //geohud cardinalchant on|off — main GEO only, off in towns: N blue crit, E red MAB, S green macc, W yellow MB')
    chat('  //geohud colorblind on|off — X on red rings, O on green rings')
    chat('  //geohud rings status     — native ring module hook/device state')
    chat('  //geohud mobs on|off      — nearby mob list under the HUD (off by default)')
    chat('  //geohud profile          — show the current character/job settings files')
    chat('  //geohud save char        — save HUD/rings/orbs toggles for this character (all jobs)')
    chat('  Drag the HUD to reposition. + tagged (potency on), ! in bubble with no GEO hate.')
end

-- GEO-HUD 1.6 copied BCRings.dll into Windower/plugins. 1.7 draws from the
-- addon folder, so that leftover plugin would double-draw if it stayed loaded.
local legacy_plugin_deadline = 0

local function windower_root()
    local root = windower.windower_path
    if type(root) ~= 'string' or root == '' then
        root = windower.addon_path:gsub('[/\\]+$', ''):gsub('[/\\]addons[/\\][^/\\]+$', '')
    end
    return root:gsub('\\', '/'):gsub('/+$', '')
end

local function legacy_bcrings_path()
    return windower_root() .. '/plugins/BCRings.dll'
end

local function file_exists(path)
    local handle = io.open(path, 'rb')
    if not handle then
        return false
    end
    handle:close()
    return true
end

local function try_delete_legacy_bcrings()
    local path = legacy_bcrings_path()
    local existed = file_exists(path)
    pcall(os.remove, path)
    if file_exists(path) then
        return 'busy'
    end
    if existed then
        return 'deleted'
    end
    return 'absent'
end

local function finish_legacy_plugin_cleanup(result)
    legacy_plugin_deadline = 0
    if result == 'deleted' then
        chat('Removed leftover Windower/plugins/BCRings.dll. Rings now load from the addon folder.')
    elseif result == 'busy' then
        chat('Could not delete Windower/plugins/BCRings.dll while it is loaded. Close Windower and it will be removed next load.')
    end
end

local function poll_legacy_plugin_cleanup()
    if legacy_plugin_deadline <= 0 then
        return
    end
    local result = try_delete_legacy_bcrings()
    if result ~= 'busy' then
        finish_legacy_plugin_cleanup(result)
    elseif os.clock() >= legacy_plugin_deadline then
        finish_legacy_plugin_cleanup('busy')
    end
end

local function cleanup_legacy_bcrings()
    -- Only touch Windower/plugins if the leftover DLL is actually there.
    -- `unload BCRings` prints "Plugin is not loaded" when the file is already gone.
    if not file_exists(legacy_bcrings_path()) then
        return
    end
    windower.send_command('unload BCRings')
    local result = try_delete_legacy_bcrings()
    if result == 'deleted' then
        finish_legacy_plugin_cleanup(result)
    elseif result == 'busy' then
        legacy_plugin_deadline = os.clock() + 5
    end
end

-- ── events ─────────────────────────────────────────────────────────────────

windower.register_event('load', function()
    build_spells()
    refresh_ui()
    logged_in = windower.ffxi.get_info().logged_in
    hud:hide()
    hide_hud_panel()
    hide_hp_bar()
    cleanup_legacy_bcrings()
    if settings.rings or settings.cardinal_chant then
        start_rings()
        if not rings_available() then
            chat('Ground rings failed: ' .. tostring(rings_load_error))
        end
    end
    if logged_in then
        chat('Loaded. //geohud help')
    end
end)

windower.register_event('unload', function()
    stop_rings()
    profile.save()
    hide_orb()
    hide_indi_orb()
    hide_entrust_orbs()
    hide_hp_bar()
end)

windower.register_event('login', function()
    logged_in = true
    refresh_ui()
    build_spells()
    profile.capture()
    profile.apply()
    profile.apply_visuals()
    if settings.rings or settings.cardinal_chant then
        start_rings()
    end
end)

windower.register_event('logout', function()
    logged_in = false
    reset_zone()
end)

windower.register_event('zone change', function()
    reset_zone()
end)

windower.register_event('job change', function()
    clear_tags()
    last_spell = nil
    last_spell_element = nil
    last_spell_is_buff = false
    last_luopan_id = nil
    local char, job = profile.who()
    if char == profile.char and job == profile.job then
        return
    end
    profile.flush()
    profile.apply_snap(settings, profile.base)
    profile.apply()
    profile.apply_visuals()
end)

windower.register_event('action', function(act)
    if not logged_in or not act then return end
    note_indi_from_action(act)
    local _, geo_id, we_are_geo = resolve_bubble()
    if we_are_geo then
        local player = windower.ffxi.get_player()
        local me = windower.ffxi.get_mob_by_target('me')
        geo_id = (me and me.id) or (player and player.id) or geo_id
    elseif last_geo_id then
        geo_id = last_geo_id
    end
    apply_action_tags(act, geo_id)
end)

windower.register_event('gain buff', function(id)
    if id == C.colure then
        last_hud = ''
    end
end)

windower.register_event('lose buff', function(id)
    if id == C.colure and watching_own_indi() and (os.clock() - (last_indi_cast or 0)) > 2.5 then
        last_indi = nil
        last_indi_element = nil
        last_indi_status = nil
        last_hud = ''
        hide_indi_orb()
    end
end)

windower.register_event('incoming chunk', function(id, original, modified, injected)
    if injected or not logged_in then return end

    if id == 0x00E then
        -- HP/status bytes are only valid when mask bit 2 is set; otherwise they
        -- are zeroed and would look like a death. Hide bit also fires for
        -- out-of-range, which must not clear enmity.
        local mob_id = original:unpack('I', 5)
        local mask = original:byte(11)
        local hp_included = bit.band(mask, 0x04) ~= 0
        if hp_included then
            local hpp = original:byte(31)
            local status = original:byte(32)
            if hpp == 0 or status == 2 or status == 3 then
                untag_mob(mob_id)
                if last_luopan_id and mob_id == last_luopan_id then
                    last_luopan_id = nil
                    bubble_alive = false
                    bubble_hpp = 0
                end
            end
        end
    elseif id == 0x067 or id == 0x068 then
        local ok, packet = pcall(packets.parse, 'incoming', original)
        if not ok or not packet then return end
        local msg_type = packet['Message Type']
        if msg_type ~= 0x04 then return end
        local pet_idx = packet['Pet Index']
        local owner_idx = packet['Owner Index']
        -- 0x067 swaps pet/owner relative to 0x068.
        if id == 0x067 then
            pet_idx, owner_idx = owner_idx, pet_idx
        end
        local player = windower.ffxi.get_player()
        if not player then return end
        if owner_idx ~= player.index and pet_idx ~= player.index then return end
        if (pet_idx == 0) then
            last_luopan_id = nil
            bubble_alive = false
            bubble_hpp = 0
        elseif packet['Current HP%'] then
            bubble_hpp = packet['Current HP%']
        end
    elseif id == 0x076 then
        party_colure = {}
        party_widened = {}
        party_slot_ids = {}
        saw_party_buffs = true
        for k = 0, 4 do
            local pid = original:unpack('I', k * 48 + 5)
            if pid and pid ~= 0 then
                party_slot_ids[pid] = true
                local has_colure, has_widen = false, false
                for n = 1, 32 do
                    local buff = original:byte(k * 48 + 5 + 16 + n - 1)
                        + 256 * (math.floor(original:byte(k * 48 + 5 + 8 + math.floor((n - 1) / 4)) / 4 ^ ((n - 1) % 4)) % 4)
                    if buff == C.colure then
                        has_colure = true
                    elseif buff == C.widened then
                        has_widen = true
                    end
                end
                party_colure[pid] = has_colure
                party_widened[pid] = has_widen
            end
        end
        refresh_entrusts()
    end
end)

windower.register_event('ipc message', on_ipc)

windower.register_event('prerender', function()
    poll_legacy_plugin_cleanup()
    if not logged_in or hidden then
        hud:hide()
        hide_orb()
        hide_indi_orb()
        hide_entrust_orbs()
        hide_hp_bar()
        hide_hud_panel()
        submit_rings()
        chant.submit()
        return
    end
    local info = windower.ffxi.get_info()
    if not info or not info.logged_in then return end

    local me = windower.ffxi.get_mob_by_target('me')
    if not me or (tonumber(me.status) or 0) == 4 then
        hud:hide()
        hide_orb()
        hide_indi_orb()
        hide_entrust_orbs()
        hide_hp_bar()
        hide_hud_panel()
        submit_rings()
        chant.submit()
        return
    end

    refresh_indi()
    refresh_entrusts()
    local luopan, geo_id, we_are_geo = resolve_bubble()
    update_orb(luopan ~= nil)
    update_indi_orb(last_indi ~= nil or watched_geo_has_colure())
    update_entrust_orbs()
    update_hp_bar(luopan ~= nil, luopan and luopan.hpp or 0)
    update_hud_panel()
    local now = os.clock()

    if now - last_scan >= 0.12 then
        last_scan = now
        refresh_ui()
        prune_tags()
        scan_nearby(me, luopan)
        update_hud(luopan, geo_id, we_are_geo)
        send_ipc(luopan, geo_id)
    end

    -- Lua sends the ring list; the native module reads live posed coords.
    if settings.rings and not hidden then
        submit_rings(luopan)
    end
    chant.submit()
end)

windower.register_event('addon command', function(command, ...)
    command = command and command:lower() or 'help'
    local args = {...}

    if command == 'help' then
        print_help()
    elseif command == 'profile' then
        local char, job = profile.who()
        if not char then
            chat('No character logged in.')
            return
        end
        local char_file = profile.path(char)
        local job_file = job and profile.path(char, job)
        chat(('Profile %s / %s'):format(char, job or '-'))
        chat('  Character file: ' .. char_file .. (files.new(char_file):exists() and '' or ' (none yet)'))
        if job_file then
            chat('  Job file: ' .. job_file .. (files.new(job_file):exists() and '' or ' (none yet)'))
        end
        chat(('  hud=%s  orbs=%s  rings=%s  chant=%s  mobs=%s'):format(
            settings.show_hud ~= false and 'on' or 'off',
            settings.orbs ~= false and 'on' or 'off',
            settings.rings and 'on' or 'off',
            settings.cardinal_chant and 'on' or 'off',
            settings.show_mobs and 'on' or 'off'))
    elseif command == 'save' then
        local arg = args[1] and args[1]:lower()
        local char, job = profile.who()
        if not char then
            chat('Log in first.')
            return
        end
        if arg == 'char' or arg == 'character' then
            profile.write(profile.path(char), profile.snapshot(settings))
            chat('Saved character defaults to ' .. profile.path(char))
        else
            profile.save()
            chat('Saved job settings to ' .. profile.path(char, job or nil))
        end
    elseif command == 'show' then
        hidden = false
        settings.always_show = true
        settings.show_hud = true
        profile.save()
        chat('HUD shown.')
    elseif command == 'colorblind' or command == 'colourblind' then
        local arg = args[1] and args[1]:lower()
        if arg == 'on' then settings.colorblind = true
        elseif arg == 'off' then settings.colorblind = false
        else settings.colorblind = not settings.colorblind end
        profile.save()
        apply_colorblind()
        chat('Colorblind rings ' .. (settings.colorblind and 'on' or 'off') .. '  (O = green, X = red).')
    elseif command == 'hide' then
        hidden = true
        hud:hide()
        hide_orb()
        hide_indi_orb()
        hide_entrust_orbs()
        hide_hp_bar()
        hide_hud_panel()
        submit_rings()
        chant.submit()
        chat('HUD and rings hidden. //geohud show to restore.')
    elseif command == 'hud' then
        local arg = args[1] and args[1]:lower()
        if arg == 'on' then settings.show_hud = true
        elseif arg == 'off' then settings.show_hud = false
        else settings.show_hud = not (settings.show_hud ~= false) end
        if settings.show_hud then
            hidden = false
        end
        profile.save()
        if settings.show_hud == false then
            hud:hide()
            hide_orb()
            hide_indi_orb()
            hide_entrust_orbs()
            hide_hp_bar()
            hide_hud_panel()
        end
        chat('HUD ' .. (settings.show_hud ~= false and 'on' or 'off') .. (settings.show_hud == false and ' (rings still tracking)' or '') .. '.')
    elseif command == 'reset' then
        clear_tags()
        chat('Cleared enmity tags.')
    elseif command == 'orbs' or command == 'orb' then
        local arg = args[1] and args[1]:lower()
        if arg == 'on' then settings.orbs = true
        elseif arg == 'off' then settings.orbs = false
        else settings.orbs = not settings.orbs end
        profile.save()
        if settings.orbs == false then
            hide_orb()
            hide_indi_orb()
            hide_entrust_orbs()
        end
        chat('Bubble animations ' .. (settings.orbs ~= false and 'on' or 'off') .. '.')
    elseif command == 'radius' then
        local n = tonumber(args[1])
        if not n or n < 1 or n > 30 then
            chat('Current radius: ' .. tostring(settings.radius) .. '  (//geohud radius 6)')
            return
        end
        settings.radius = n
        profile.save()
        chat('Bubble radius set to ' .. n .. ' yalms.')
    elseif command == 'rings' or command == 'ring' then
        local arg = args[1] and args[1]:lower()
        if arg == 'test' or arg == 'self' then
            settings.rings = true
            hidden = false
            start_rings()
            ring_test_until = os.clock() + 30
            submit_rings()
            chat('Ring test: green ring under you for 30s.')
            return
        end
        if arg == 'status' then
            report_rings()
            return
        end
        if arg == 'colorblind' or arg == 'colourblind' then
            local mode = args[2] and tostring(args[2]):lower()
            if mode == 'on' then settings.colorblind = true
            elseif mode == 'off' then settings.colorblind = false
            else settings.colorblind = not settings.colorblind end
            profile.save()
            apply_colorblind()
            chat('Colorblind rings ' .. (settings.colorblind and 'on' or 'off') .. '  (O = green, X = red).')
            return
        end
        if arg == 'on' then settings.rings = true
        elseif arg == 'off' then settings.rings = false
        else settings.rings = not settings.rings end
        profile.save()
        if settings.rings or settings.cardinal_chant then
            start_rings()
            if settings.rings then
                submit_rings()
            elseif rings_available() then
                _GEORings.clear()
                publish_rings()
            end
            chant.submit()
        else
            stop_rings()
        end
        chat('Ground rings ' .. (settings.rings and 'on' or 'off') .. '.')
    elseif command == 'cardinalchant' or command == 'cardinal' or command == 'chant' or command == 'cc' then
        local arg = args[1] and args[1]:lower()
        if arg ~= 'off' and not chant.main_geo() then
            settings.cardinal_chant = false
            profile.save()
            chant.submit()
            chat('Cardinal Chant is only available on main GEO.')
            return
        end
        if arg == 'on' then settings.cardinal_chant = true
        elseif arg == 'off' then settings.cardinal_chant = false
        else settings.cardinal_chant = not settings.cardinal_chant end
        profile.save()
        if settings.rings or chant.enabled() then
            start_rings()
            chant.submit()
        else
            stop_rings()
        end
        chat('Cardinal Chant circle ' .. (settings.cardinal_chant and 'on' or 'off')
            .. '  (N blue crit, E red MAB, S green macc, W yellow MB).')
    elseif command == 'ipc' then
        local arg = args[1] and args[1]:lower()
        if arg == 'on' then settings.ipc = true
        elseif arg == 'off' then settings.ipc = false
        else settings.ipc = not settings.ipc end
        profile.save()
        chat('IPC sharing ' .. (settings.ipc and 'on' or 'off') .. '.')
    elseif command == 'mobs' or command == 'list' or command == 'moblist' then
        local arg = args[1] and args[1]:lower()
        if arg == 'on' then settings.show_mobs = true
        elseif arg == 'off' then settings.show_mobs = false
        else settings.show_mobs = not settings.show_mobs end
        profile.save()
        last_hud = ''
        chat('Nearby mob list ' .. (settings.show_mobs and 'on' or 'off') .. '.')
    elseif command == 'debug' then
        debug_on = not debug_on
        chat('Debug ' .. (debug_on and 'on' or 'off') .. '.')
        local luopan, geo_id = resolve_bubble()
        local me = windower.ffxi.get_mob_by_target('me')
        chat(('luopan=%s hpp=%s geo_id=%s me=%s tags=%d'):format(
            luopan and luopan.name or 'nil',
            luopan and tostring(luopan.hpp) or '-',
            tostring(geo_id),
            me and me.name or 'nil',
            (function() local n=0 for _ in pairs(tagged) do n=n+1 end return n end)()))
    elseif command == 'cam' then
        local key = args[1] and args[1]:lower()
        local val = args[2]
        if key == 'reverse' then
            settings.camera.reverse = not settings.camera.reverse
            chat('Camera heading reverse: ' .. tostring(settings.camera.reverse))
        elseif key == 'axis' and (val == 'y' or val == 'z') then
            settings.camera.height_axis = val
            chat('Height axis: ' .. val)
        elseif key and settings.camera[key] ~= nil and tonumber(val) then
            settings.camera[key] = tonumber(val)
            chat(('camera.%s = %s'):format(key, val))
        else
            chat(('cam back=%.1f height=%.1f fov=%.0f squash=%.2f reverse=%s axis=%s'):format(
                settings.camera.back, settings.camera.height, settings.camera.fov,
                settings.camera.squash, tostring(settings.camera.reverse), settings.camera.height_axis))
            chat('//geohud cam back|height|fov|squash <n>  |  //geohud cam reverse  |  //geohud cam axis y|z')
        end
        profile.save()
    else
        print_help()
    end
end)
