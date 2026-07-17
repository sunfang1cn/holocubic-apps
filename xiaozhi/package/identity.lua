local M = {}

local DEFAULT_BOARD_TYPE = "bread-compact-wifi-lcd"
local DEFAULT_BOARD_NAME = "bread-compact-wifi-lcd"
local DEFAULT_FW_VERSION = "1.7.5"
local cached_mac = nil

local function json_escape(text)
  text = tostring(text or "")
  text = text:gsub("\\", "\\\\")
  text = text:gsub('"', '\\"')
  text = text:gsub("\n", "\\n")
  text = text:gsub("\r", "\\r")
  return text
end

local function normalize_mac(text)
  local hex = tostring(text or ""):lower():gsub("[^0-9a-f]", "")
  if #hex < 12 then
    return nil
  end
  hex = hex:sub(1, 12)
  if hex == "000000000000" or hex == "ffffffffffff" then
    return nil
  end
  local out = {}
  for i = 1, 12, 2 do
    out[#out + 1] = hex:sub(i, i + 1)
  end
  return table.concat(out, ":")
end

function M.device_id()
  if cached_mac then
    return cached_mac
  end
  if wifi and wifi.sta and wifi.sta.getmac then
    local ok, mac = pcall(wifi.sta.getmac)
    if ok then
      mac = normalize_mac(mac)
      if mac then
        cached_mac = mac
        return cached_mac
      end
    end
  end
  if sys and sys.mac then
    local ok, mac = pcall(sys.mac)
    if ok then
      mac = normalize_mac(mac)
      if mac then
        cached_mac = mac
        return cached_mac
      end
    end
  end
  return nil
end

function M.client_id(mac)
  mac = normalize_mac(mac or M.device_id())
  if not mac then
    return nil
  end
  local hex = mac:gsub(":", "")
  return "00000000-0000-4000-8000-" .. hex
end

function M.board_type(cfg)
  return tostring((cfg and cfg.BOARD_TYPE) or DEFAULT_BOARD_TYPE)
end

function M.board_name(cfg)
  return tostring((cfg and cfg.BOARD_NAME) or DEFAULT_BOARD_NAME)
end

function M.firmware_version(cfg)
  return tostring((cfg and cfg.FIRMWARE_VERSION) or DEFAULT_FW_VERSION)
end

function M.user_agent(cfg)
  return M.board_name(cfg) .. "/" .. M.firmware_version(cfg)
end

function M.http_headers(cfg)
  local mac = M.device_id()
  local client_id = M.client_id(mac)
  if not mac or not client_id then
    return nil, "device mac unavailable"
  end
  return {
    ["Activation-Version"] = "1",
    ["Device-Id"] = mac,
    ["Client-Id"] = client_id,
    ["User-Agent"] = M.user_agent(cfg),
    ["Accept-Language"] = "zh-CN",
    ["Content-Type"] = "application/json",
  }
end

function M.system_info(cfg)
  local mac = M.device_id()
  local client_id = M.client_id(mac)
  if not mac or not client_id then
    return nil, "device mac unavailable"
  end
  local usage = nil
  if sys and sys.usage then
    local ok, data = pcall(sys.usage)
    if ok and type(data) == "table" then
      usage = data
    end
  end

  return {
    version = 2,
    language = "zh-CN",
    flash_size = tonumber((cfg and cfg.FLASH_SIZE) or 16777216),
    minimum_free_heap_size = tostring((usage and usage.heap_free) or 123456),
    mac_address = mac,
    uuid = client_id,
    chip_model_name = "esp32s3",
    chip_info = {
      model = 9,
      cores = 2,
      revision = 0,
      features = 18,
    },
    application = {
      name = "xiaozhi",
      version = M.firmware_version(cfg),
      compile_time = "2026-06-24T00:00:00Z",
      idf_version = "v5.3.2",
      elf_sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    },
    partition_table = {
      {
        label = "app0",
        type = 0,
        subtype = 16,
        address = 65536,
        size = 2097152,
      },
    },
    ota = {
      label = "app0",
    },
    display = {
      monochrome = false,
      width = 320,
      height = 240,
    },
    board = {
      type = M.board_type(cfg),
      name = M.board_name(cfg),
      mac = mac,
    },
  }
end

function M.system_info_json(cfg)
  local info, info_err = M.system_info(cfg)
  if not info then
    return nil, info_err
  end
  if sjson and sjson.encode then
    local ok, text = pcall(sjson.encode, info)
    if ok and type(text) == "string" then
      return text
    end
  end

  local mac = info.mac_address
  local uuid = info.uuid
  local board_type = json_escape(M.board_type(cfg))
  local board_name = json_escape(M.board_name(cfg))
  local fw = json_escape(M.firmware_version(cfg))
  return '{"version":2,"language":"zh-CN","flash_size":16777216,' ..
    '"minimum_free_heap_size":"123456",' ..
    '"mac_address":"' .. json_escape(mac) .. '","uuid":"' .. json_escape(uuid) .. '",' ..
    '"chip_model_name":"esp32s3","chip_info":{"model":9,"cores":2,"revision":0,"features":18},' ..
    '"application":{"name":"xiaozhi","version":"' .. fw .. '","compile_time":"2026-06-24T00:00:00Z",' ..
    '"idf_version":"v5.3.2","elf_sha256":"0000000000000000000000000000000000000000000000000000000000000000"},' ..
    '"partition_table":[{"label":"app0","type":0,"subtype":16,"address":65536,"size":2097152}],' ..
    '"ota":{"label":"app0"},"display":{"monochrome":false,"width":320,"height":240},' ..
    '"board":{"type":"' .. board_type .. '","name":"' .. board_name .. '","mac":"' .. json_escape(mac) .. '"}}'
end

return M
