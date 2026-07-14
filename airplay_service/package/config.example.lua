-- Copy this file to config.lua on the SD card to override defaults.
-- Author: sunfang1cn@gmail.com
return {
  name = "HoloCubic",
  rtsp_port = 5000,
  rtsp_idle_timeout_s = 7200,
  audio_port = 6000,
  control_port = 6001,
  timing_port = 6002,
  i2s_port = 0,
  data_out_pin = 48,
  dma_buffer_count = 12,
  dma_buffer_len = 512,
  prebuffer_packets = 160,
  missing_wait_ticks = 192,
  timing_poll_ms = 100,
  timing_initial_interval_ms = 300,
  timing_interval_ms = 3000,
  audio_task_priority = 10,
  audio_task_core = -1,
  output_task_core = 0,
  debug = false,
}
