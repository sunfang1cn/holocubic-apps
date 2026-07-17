local previous = rawget(_G, "XIAOZHI_APP")
if previous and previous.stop then
  pcall(function()
    previous.stop("reload")
  end)
end

local APP_DIR = "/sd/apps/xiaozhi"

local function load_app_module(name)
  return dofile(APP_DIR .. "/" .. name .. ".lua")
end

local Config = load_app_module("config")
local Runtime = load_app_module("runtime")

local cfg = Config.load()
local app = Runtime.new(cfg, load_app_module)
local app_api = rawget(_G, "app")

XIAOZHI_APP = app
app:start()

if controller and controller.state and tmr and tmr.create then
  local controller_buttons = 0
  app.controller_exit_timer = tmr.create()
  app.controller_exit_timer:alarm(40, tmr.ALARM_AUTO, function()
    local ok, pad = pcall(function() return controller.state("ble-main") end)
    local buttons = ok and type(pad) == "table" and (tonumber(pad.buttons) or 0) or 0
    local pressed = buttons & (~controller_buttons)
    controller_buttons = buttons
    if (pressed & (4096 | 32768)) ~= 0 then
      pcall(function() app.stop("controller-exit") end)
      if app_api and app_api.exit then pcall(function() app_api.exit() end) end
    end
  end)
end
