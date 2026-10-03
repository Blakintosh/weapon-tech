-- weapon_tech_loader.lua: loads weapon_tech.dll from this map's zone folder.
--
-- Install: usermaps\<map>\ui\t7\utility\weapon_tech_loader.lua
-- Zone:    rawfile,ui/t7/utility/weapon_tech_loader.lua      (in zone_source\<map>.zone)
-- CSC:     LuiLoad("ui.t7.utility.weapon_tech_loader");      (in main() of scripts\zm\<map>.csc)
--
-- install.bat does the copy, fills in MAP_NAME and adds the zone line. The CSC line is yours to add.
-- Safe to run on every level load, and the map works if the DLL is missing: it never raises an error.
-- weapontech_active is 1 only when the DLL says it is running; scripts can check it with GetDvarInt.

local MAP_NAME = "__WT_MAPNAME__"   -- install.bat sets this. By hand: your map's folder name under usermaps\
local WORKSHOP_ID = nil             -- optional: your map's Workshop ID as a string, e.g. "1234567890"

local function SetActive(value)
    pcall(Engine.SetDvar, "weapontech_active", value)
    -- Engine.SetDvar may not create a dvar that doesn't exist yet; "set" does.
    if Engine.DvarInt and Engine.Exec and Engine.DvarInt(nil, "weapontech_active") ~= value then
        pcall(Engine.Exec, 0, "set weapontech_active " .. value)
    end
end

local function Load()
    SetActive(0)
    if MAP_NAME == "__WT_MAPNAME__" then
        return  -- not set up: set MAP_NAME above
    end

    local candidates = {
        [[.\usermaps\]] .. MAP_NAME .. [[\zone\weapon_tech.dll]],
        [[usermaps\]] .. MAP_NAME .. [[\zone\weapon_tech.dll]],
    }
    if WORKSHOP_ID then
        candidates[#candidates + 1] = [[..\..\workshop\content\311210\]] .. WORKSHOP_ID .. [[\zone\weapon_tech.dll]]
        candidates[#candidates + 1] = [[..\..\workshop\content\311210\]] .. WORKSHOP_ID .. [[\weapon_tech.dll]]
    end

    local pkg = require("package")
    for i = 1, #candidates do
        local ok, init = pcall(pkg.loadlib, candidates[i], "init")
        if ok and init then
            local called, active = pcall(init, true)  -- init(true) is true when weapon_tech is running
            if called and active == true then
                SetActive(1)
            end
            return
        end
    end
end

pcall(Load)
