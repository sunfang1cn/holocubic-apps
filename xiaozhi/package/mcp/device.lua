local M = {}

local NTP_SERVERS = {
  "ntp.aliyun.com",
  "time.cloudflare.com",
  "pool.ntp.org",
}

M.tools = {
  {
    name = "device.get_status",
    description = "读取设备型号、固件、网络和内存状态。",
    inputSchema = { type = "object", additionalProperties = false },
  },
  {
    name = "device.list_apps",
    description = "列出设备上已经安装并可启动的应用。",
    inputSchema = { type = "object", additionalProperties = false },
  },
  {
    name = "device.launch_app",
    description = "按应用 ID 启动设备上已安装的应用。调用前应先使用 device.list_apps 获取 ID。",
    inputSchema = {
      type = "object",
      properties = { app_id = { type = "string", description = "应用 ID" } },
      required = { "app_id" },
      additionalProperties = false,
    },
  },
  {
    name = "device.sync_time",
    description = "立即通过 NTP 同步系统时间。可选传入 server 指定 NTP 服务器。",
    inputSchema = {
      type = "object",
      properties = { server = { type = "string", description = "可选 NTP 服务器域名" } },
      additionalProperties = false,
    },
  },
  {
    name = "device.set_brightness",
    description = "设置屏幕亮度，范围 0 到 100。",
    inputSchema = {
      type = "object",
      properties = {
        brightness = {
          type = "integer",
          minimum = 0,
          maximum = 100,
          description = "屏幕亮度百分比，0 到 100。",
        },
      },
      required = { "brightness" },
      additionalProperties = false,
    },
  },
  {
    name = "device.set_bluetooth",
    description = "开启或关闭蓝牙手柄服务。",
    inputSchema = {
      type = "object",
      properties = {
        enabled = {
          type = "boolean",
          description = "true 开启蓝牙，false 关闭蓝牙。",
        },
      },
      required = { "enabled" },
      additionalProperties = false,
    },
  },
}

local function clamp(value, min_value, max_value)
  local num = tonumber(value)
  if not num then
    return nil
  end
  if num < min_value then
    num = min_value
  elseif num > max_value then
    num = max_value
  end
  return math.floor(num + 0.5)
end

local function get_brightness()
  if not sys or not sys.getbrightness then
    return nil, "sys.getbrightness unavailable"
  end
  local ok, level = pcall(sys.getbrightness)
  if not ok then
    return nil, tostring(level or "brightness read failed")
  end
  return clamp(level, 0, 100) or 0
end

local function set_brightness(level)
  local target = clamp(level, 0, 100)
  if not target then
    return nil, "invalid brightness"
  end
  if not sys or not sys.setbrightness then
    return nil, "sys.setbrightness unavailable"
  end
  local ok_call, ok_set, err = pcall(sys.setbrightness, target)
  if not ok_call or ok_set == false then
    return nil, tostring(err or ok_set or "brightness set failed")
  end
  return { accepted = true, brightness = target }
end

local function get_bluetooth_status()
  if not gamepad or not gamepad.state then
    return {
      available = false,
      enabled = false,
      connected = false,
      connecting = false,
      status = "unavailable",
    }
  end
  local ok, state = pcall(gamepad.state)
  if not ok or type(state) ~= "table" then
    return {
      available = true,
      enabled = false,
      connected = false,
      connecting = false,
      status = "error",
      error = tostring(state or "gamepad.state failed"),
    }
  end
  local connected = state.connected and true or false
  local connecting = state.connecting and true or false
  local enabled = state.started and true or false
  local status = "off"
  if connected then
    status = "connected"
  elseif connecting then
    status = "connecting"
  elseif enabled then
    status = "on"
  end
  return {
    available = true,
    enabled = enabled,
    connected = connected,
    connecting = connecting,
    status = status,
    name = state.name,
    address = state.address or state.last_address,
    profile = state.profile,
  }
end

local function set_bluetooth_enabled(enabled)
  enabled = enabled and true or false
  if not gamepad then
    return nil, "gamepad api unavailable"
  end
  if enabled then
    if gamepad.off then
      pcall(gamepad.off)
    end
    if not gamepad.start then
      return nil, "gamepad.start unavailable"
    end
    local ok_call, ok_start, err = pcall(function()
      return gamepad.start({
        clear_bonds = false,
        debug = false,
      })
    end)
    if not ok_call or ok_start == false then
      return nil, tostring(err or ok_start or "bluetooth start failed")
    end
  else
    if gamepad.off then
      pcall(gamepad.off)
    end
    if not gamepad.stop then
      return nil, "gamepad.stop unavailable"
    end
    local ok_call, err = pcall(gamepad.stop)
    if not ok_call then
      return nil, tostring(err or "bluetooth stop failed")
    end
  end
  local status = get_bluetooth_status()
  status.accepted = true
  status.enabled = enabled
  return status
end

local function installed_apps()
  if not app or not app.list then
    return nil, "app api unavailable"
  end
  local ok, list = pcall(app.list)
  if not ok or type(list) ~= "table" then
    return nil, tostring(list or "failed to list apps")
  end
  local result = {}
  for _, item in ipairs(list) do
    if type(item) == "table" and item.id then
      result[#result + 1] = {
        id = tostring(item.id),
        name = tostring(item.name or item.id),
        description = tostring(item.description or ""),
      }
    end
  end
  return result
end

local function sync_time(server)
  if not time or type(time.initntp) ~= "function" then
    return nil, "time.initntp unavailable"
  end
  server = type(server) == "string" and server:match("^%s*(.-)%s*$") or ""
  if server == "" then
    server = NTP_SERVERS[1]
  end
  if not server:match("^[%w_.%-]+$") then
    return nil, "invalid ntp server"
  end
  local ok, result = pcall(time.initntp, server)
  if not ok then
    return nil, tostring(result or "ntp request failed")
  end
  if result == false then
    return nil, "ntp request rejected"
  end
  return { accepted = true, server = server }
end

local function launch_app(app_id, ctx)
  if type(app_id) ~= "string" or app_id == "" or #app_id > 64 or not app_id:match("^[%w_.%-]+$") then
    return nil, "invalid app_id"
  end
  local list, err = installed_apps()
  if not list then return nil, err end
  local found = false
  for _, item in ipairs(list) do
    if item.id == app_id then found = true break end
  end
  if not found then return nil, "app not installed: " .. app_id end

  if not tmr or not tmr.create then
    return nil, "timer api unavailable"
  end
  local timer = tmr.create()
  timer:alarm(250, tmr.ALARM_SINGLE, function(instance)
    pcall(function() instance:unregister() end)
    if ctx.before_app_exit then
      local prepare_ok, prepare_err = pcall(ctx.before_app_exit)
      if not prepare_ok then print("[xiaozhi] mcp app switch prepare failed", tostring(prepare_err)) end
    end
    local ok_launch, launch_err = app.launch(app_id)
    if not ok_launch then print("[xiaozhi] mcp launch failed", app_id, tostring(launch_err)) end
  end)
  return { accepted = true, app_id = app_id }
end

M.handlers = {
  ["device.get_status"] = function(arguments, ctx)
    local cfg = ctx.cfg or {}
    local ok, identity = pcall(dofile, (cfg.APP_DIR or "/sd/apps/xiaozhi") .. "/identity.lua")
    if not ok or type(identity) ~= "table" or not identity.system_info then
      return ctx.error_result("identity module unavailable")
    end
    local status_ok, status = pcall(identity.system_info, cfg)
    if not status_ok or type(status) ~= "table" then
      return ctx.error_result(status or "failed to read device status")
    end
    if wifi and wifi.sta and wifi.sta.getip then
      local ok_ip, ip = pcall(wifi.sta.getip)
      if ok_ip then status.ip_address = ip end
    end
    local brightness = get_brightness()
    if type(status.display) ~= "table" then
      status.display = {}
    end
    status.display.brightness = brightness
    status.bluetooth = get_bluetooth_status()
    return ctx.text_result(status)
  end,

  ["device.list_apps"] = function(arguments, ctx)
    local list, err = installed_apps()
    return list and ctx.text_result({ apps = list }) or ctx.error_result(err)
  end,

  ["device.sync_time"] = function(arguments, ctx)
    local result, err = sync_time(arguments.server)
    return result and ctx.text_result(result) or ctx.error_result(err)
  end,

  ["device.launch_app"] = function(arguments, ctx)
    local result, err = launch_app(arguments.app_id, ctx)
    return result and ctx.text_result(result) or ctx.error_result(err)
  end,

  ["device.set_brightness"] = function(arguments, ctx)
    local result, err = set_brightness(arguments.brightness)
    return result and ctx.text_result(result) or ctx.error_result(err)
  end,

  ["device.set_bluetooth"] = function(arguments, ctx)
    local result, err = set_bluetooth_enabled(arguments.enabled)
    return result and ctx.text_result(result) or ctx.error_result(err)
  end,
}

return M
