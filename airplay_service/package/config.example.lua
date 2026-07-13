-- Copy this file to config.lua on the SD card to override defaults.
return {
  name = "HoloCubic",
  rtsp_port = 5000,
  audio_port = 6000,
  control_port = 6001,
  timing_port = 6002,
  i2s_port = 0,
  data_out_pin = 48,
  dma_buffer_count = 12,
  dma_buffer_len = 512,
  prebuffer_packets = 48,
  missing_wait_ticks = 12,
  audio_task_priority = 8,
  audio_task_core = 1,
  output_task_core = 0,
  debug = false,
}
