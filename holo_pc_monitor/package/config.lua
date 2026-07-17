local config = {}

config.host = "192.168.0.80"
config.port = 80
config.path = "/sse"
config.layout = "dashboard"
config.cpu_name = "CPU"
config.gpu_name = "GPU"
config.accent_color = 0xE7C21D

config.timeout_ms = 7000
config.reconnect_ms = 2000
config.stale_ms = 5000
config.watchdog_ms = 1000
config.serial_log = true

config.history_points = 48

config.thresholds = {
  warm_temp = 70,
  hot_temp = 85,
  warm_load = 75,
  hot_load = 92
}

config.metrics = {
  {
    id = "cpu_usage",
    title = "CPU",
    unit = "%",
    kind = "percent",
    aliases = { "CPU Usage", "CPU Utilization", "CPU" }
  },
  {
    id = "gpu_usage",
    title = "GPU",
    unit = "%",
    kind = "percent",
    aliases = { "GPU Usage", "GPU1 Usage", "GPU Utilization", "GPU" }
  },
  {
    id = "memory_usage",
    title = "RAM",
    unit = "%",
    kind = "percent",
    aliases = { "Memory Usage", "Memory Utilization", "RAM Usage", "Memory" }
  },
  {
    id = "cpu_clock",
    title = "CPU Clock",
    unit = "MHz",
    kind = "clock",
    aliases = { "CPU Frequency", "CPU Clock", "CPU Core Clock" },
    min_valid = 1
  },
  {
    id = "cpu_voltage",
    title = "CPU Voltage",
    unit = "V",
    kind = "voltage",
    aliases = { "CPU Voltage", "CPU Core Voltage", "Vcore", "CPU Vcore", "CPU VID" },
    min_valid = 0.01
  },
  {
    id = "cpu_power",
    title = "CPU Power",
    unit = "W",
    kind = "power",
    aliases = { "CPU Package Power", "CPU Power", "CPU PPT" },
    min_valid = 0.01
  },
  {
    id = "cpu_name", title = "CPU Name", kind = "text", aliases = { "CPU Name" }
  },
  {
    id = "gpu_name", title = "GPU Name", kind = "text", aliases = { "GPU Name" }
  },
  {
    id = "memory_used", title = "Used Memory", unit = "MB", kind = "memory", aliases = { "Used Memory" }, min_valid = 0
  },
  {
    id = "memory_free", title = "Free Memory", unit = "MB", kind = "memory", aliases = { "Free Memory" }, min_valid = 0
  },
  {
    id = "network_upload", title = "Network Upload", unit = "KB/s", kind = "network", aliases = { "Network Upload 1", "Network Upload 2", "Network Upload 3", "Network Upload 4", "Network Upload 5", "Network Upload 6", "Network Upload 7", "Network Upload 8" }, min_valid = 0.01
  },
  {
    id = "network_download", title = "Network Download", unit = "KB/s", kind = "network", aliases = { "Network Download 1", "Network Download 2", "Network Download 3", "Network Download 4", "Network Download 5", "Network Download 6", "Network Download 7", "Network Download 8" }, min_valid = 0.01
  },
  {
    id = "gpu_clock",
    title = "GPU Clock",
    unit = "MHz",
    kind = "clock",
    aliases = { "GPU Frequency", "GPU Clock", "GPU Core Clock" },
    min_valid = 1
  },
  {
    id = "cpu_temp",
    title = "CPU Temp",
    unit = "C",
    kind = "temperature",
    aliases = { "CPU 二极管", "CPU Diode", "CPU Temperature", "CPU Package" },
    min_valid = 1
  },
  {
    id = "gpu_temp",
    title = "GPU Temp",
    unit = "C",
    kind = "temperature",
    aliases = { "GPU Temperature", "GPU Diode", "GPU1 Temperature" },
    min_valid = 1
  },
  {
    id = "fan",
    title = "Fan",
    unit = "RPM",
    kind = "speed",
    aliases = { "CPU Fan", "CPU Fan Alternate", "GPU Fan", "Chassis Fan 1", "Chassis Fan 2", "Chassis Fan", "Fan" },
    min_valid = 1
  }
}

return config
