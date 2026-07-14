local M = {}

function M.new(cfg, load_module)
  local State = load_module("state")
  local Ui = load_module("ui")
  local Audio = load_module("audio")
  local Protocol = load_module("protocol")
  local Activation = load_module("activation")
  local Mcp = load_module("mcp")

  local self = {
    cfg = cfg,
    state = State.new(),
    ui = Ui.new(cfg),
    audio = nil,
    protocol = nil,
    activation = nil,
    mcp = nil,
    timer = nil,
    wake_open_timer = nil,
    listening_mode = State.LISTEN_AUTO,
    pending_wake_word = nil,
    stopped = false,
  }

  local function set_state(to)
    return self.state:set(to)
  end

  local function refresh_metrics()
    if not self.ui then
      return
    end
    local ai = self.audio and self.audio:info() or {}
    local pi = self.protocol and self.protocol:info() or {}
    local wake = "OFF"
    if ai.wake_ready then
      wake = "READY"
    elseif ai.wake_missing then
      wake = "MISS"
    end
    local network = pi.opened and "WS" or (pi.connected and "NET" or "NET")
    local counter = "lv " .. tostring(ai.level or 0) ..
      "  det " .. tostring(ai.detect_count or 0) ..
      "  tx " .. tostring(ai.send_frames or 0) ..
      "  rx " .. tostring(ai.play_count or 0)
    self.ui:set_metrics({
      network = network,
      audio = (ai.mode or "off"),
      wake = wake,
      counter = counter,
    })
  end

  local function alert(status, message, emotion)
    self.ui:alert(status or "错误", message or "", emotion or "circle_xmark")
  end

  local function open_audio_channel_now()
    local ok = self.protocol:open_audio_channel()
    if not ok then
      local msg = self.protocol.last_error or "server not connected"
      if msg == "websocket config missing" then
        msg = "未配置 websocket"
      end
      alert("连接失败", msg, "cloud_slash")
      set_state(State.IDLE)
      return false
    end
    return true
  end

  local function open_audio_channel()
    if not self.protocol then
      alert("错误", "protocol missing", "circle_xmark")
      set_state(State.IDLE)
      return false
    end
    if self.protocol:is_audio_channel_opened() then
      set_state(State.LISTENING)
      return true
    end
    set_state(State.CONNECTING)
    self.ui:set_chat_message("system", "")
    return open_audio_channel_now()
  end

  local function open_audio_channel_deferred()
    if not self.protocol then
      alert("错误", "protocol missing", "circle_xmark")
      set_state(State.IDLE)
      return false
    end
    if self.protocol:is_audio_channel_opened() then
      set_state(State.LISTENING)
      return true
    end
    set_state(State.CONNECTING)
    self.ui:set_chat_message("system", "")
    if self.wake_open_timer then
      pcall(function() self.wake_open_timer:stop() end)
      pcall(function() self.wake_open_timer:unregister() end)
      self.wake_open_timer = nil
    end
    if tmr and tmr.create then
      self.wake_open_timer = tmr.create()
      self.wake_open_timer:alarm(1, tmr.ALARM_SINGLE, function()
        self.wake_open_timer = nil
        if not self.stopped and self.state.state == State.CONNECTING then
          open_audio_channel_now()
        end
      end)
      return true
    end
    return open_audio_channel_now()
  end

  local function start_listening(mode)
    self.listening_mode = mode or State.LISTEN_AUTO
    local s = self.state.state
    if s == State.IDLE then
      return open_audio_channel()
    elseif s == State.SPEAKING then
      self.protocol:send_abort_speaking("none")
      set_state(State.LISTENING)
      return true
    elseif s == State.LISTENING then
      return true
    end
    return false
  end

  local function stop_listening()
    local s = self.state.state
    if s == State.LISTENING and self.protocol then
      self.protocol:send_stop_listening()
      set_state(State.IDLE)
    elseif s == State.SPEAKING and self.protocol then
      self.protocol:send_abort_speaking("none")
      set_state(State.IDLE)
    elseif s == State.CONNECTING and self.protocol then
      self.protocol:close_audio_channel(false)
      set_state(State.IDLE)
    end
  end

  local function wake_word_invoke(wake_word)
    local s = self.state.state
    print("[xiaozhi] wake detected", tostring(wake_word), "state=" .. tostring(s))
    self.pending_wake_word = wake_word or cfg.WAKE_WORD
    if s == State.IDLE then
      if not self.audio:begin_wake_bridge() then
        self.audio:stop_i2s()
      end
      self.ui:show_notification("你好小智", 1200)
      return open_audio_channel_deferred()
    elseif s == State.SPEAKING or s == State.LISTENING then
      if self.protocol then
        self.protocol:send_abort_speaking("wake_word_detected")
      end
      set_state(State.LISTENING)
    elseif s == State.ACTIVATING then
      set_state(State.IDLE)
    end
    return true
  end

  local function toggle_chat()
    local s = self.state.state
    if s == State.IDLE then
      start_listening(State.LISTEN_AUTO)
    elseif s == State.SPEAKING then
      if self.protocol then
        self.protocol:send_abort_speaking("none")
      end
      set_state(State.IDLE)
    elseif s == State.LISTENING or s == State.CONNECTING then
      stop_listening()
    elseif s == State.ACTIVATING then
      set_state(State.IDLE)
    end
  end

  local function on_state_changed(_, new_state)
    self.ui:on_state(new_state)
    if new_state == State.IDLE then
      self.ui:clear_chat_messages()
      self.audio:set_mode("wake")
    elseif new_state == State.CONNECTING then
      if not self.audio:is_wake_bridge() then
        self.audio:set_mode("off")
      end
    elseif new_state == State.LISTENING then
      if self.pending_wake_word and self.protocol then
        self.protocol:send_wake_word_detected(self.pending_wake_word)
        self.pending_wake_word = nil
      end
      if self.protocol then
        self.protocol:send_start_listening(self.listening_mode)
      end
      self.audio:set_mode("listen")
    elseif new_state == State.SPEAKING then
      self.audio:set_mode("speak")
    elseif new_state == State.FATAL_ERROR then
      self.audio:set_mode("off")
    end
    refresh_metrics()
  end

  local function bind_protocol()
    self.protocol:on("opened", function()
      if self.state.state == State.CONNECTING then
        set_state(State.LISTENING)
      end
    end)
    self.protocol:on("closed", function()
      if self.state.state == State.CONNECTING or self.state.state == State.LISTENING or self.state.state == State.SPEAKING then
        set_state(State.IDLE)
      end
    end)
    self.protocol:on("error", function(message)
      alert("错误", message, "cloud_slash")
      if self.state.state == State.CONNECTING then
        set_state(State.IDLE)
      end
    end)
    self.protocol:on("audio", function(opus)
      if self.state.state ~= State.SPEAKING then
        set_state(State.SPEAKING)
      end
      self.audio:play_opus(opus)
    end)
    self.protocol:on("tts_start", function()
      set_state(State.SPEAKING)
    end)
    self.protocol:on("tts_stop", function()
      if self.listening_mode == State.LISTEN_MANUAL then
        set_state(State.IDLE)
      else
        set_state(State.LISTENING)
      end
    end)
    self.protocol:on("chat", function(role, text)
      self.ui:set_chat_message(role, text)
    end)
    self.protocol:on("emotion", function(emotion)
      self.ui:set_emotion(emotion)
    end)
    self.protocol:on("alert", function(status, message, emotion)
      alert(status, message, emotion)
    end)
    self.protocol:on("mcp", function(payload)
      if self.mcp then
        self.mcp:handle(payload)
      end
    end)
  end

  local function bind_audio()
    self.audio.on_wake = wake_word_invoke
    self.audio.on_send = function(opus)
      if self.protocol and self.protocol:is_audio_channel_opened() then
        pcall(function()
          self.protocol:send_audio(opus)
        end)
      end
    end
    self.audio.on_error = function(message)
      if message == "wake model missing" then
        self.ui:show_notification("缺少唤醒模型", 2500)
      else
        self.ui:show_notification(message, 2500)
      end
    end
  end

  local function start_activation()
    if self.activation then
      self.activation:stop()
      self.activation = nil
    end
    self.activation = Activation.new(cfg)
    local started = self.activation:start(function(event, data)
      if event == "need_config" then
        self.ui:set_chat_message("system", data or "未配置 ota.url")
      elseif event == "checking" then
        set_state(State.ACTIVATING)
        self.ui:set_status("检查 OTA")
        self.ui:set_chat_message("system", "正在向小智服务端申请验证码")
      elseif event == "code" then
        set_state(State.ACTIVATING)
        local code = data and data.code or ""
        self.ui:set_emotion("thinking")
        if code ~= "" then
          self.ui:set_status("验证码 " .. code)
          self.ui:set_chat_message("system", "后台添加设备输入验证码 " .. code)
          self.ui:show_notification("验证码 " .. code, 2500)
        else
          self.ui:set_status("等待绑定")
          self.ui:set_chat_message("system", data and data.message or "等待后台设备绑定")
        end
      elseif event == "pending" then
        local code = data and data.code or ""
        if code ~= "" then
          self.ui:set_status("验证码 " .. code)
        else
          self.ui:set_status("等待绑定")
        end
      elseif event == "activated" then
        self.ui:show_notification("绑定成功", 1600)
        self.ui:set_chat_message("system", "绑定成功，正在读取服务配置")
      elseif event == "done" then
        self.protocol:start()
        self.ui:show_notification("小智已就绪", 1600)
        set_state(State.IDLE)
        refresh_metrics()
      elseif event == "failed" then
        alert("激活失败", data or "activation failed", "cloud_slash")
        set_state(State.IDLE)
        refresh_metrics()
      end
    end)
    return started
  end

  local function bind_keys()
    if not key or not key.on then
      return
    end
    local down = key.DOWN or rawget(_G, "KEY_DOWN")
    local left = key.LEFT or rawget(_G, "KEY_LEFT")
    local right = key.RIGHT or rawget(_G, "KEY_RIGHT")
    local short = key.SHORT or rawget(_G, "KEY_EVENT_SHORT")
    local start = key.START or rawget(_G, "KEY_EVENT_START")
    local long_start = key.LONG_START or rawget(_G, "KEY_EVENT_LONG_START")
    local long_repeat = key.LONG_REPEAT or rawget(_G, "KEY_EVENT_LONG_REPEAT")
    local function fire(evt)
      return evt == short or evt == start
    end
    local function long_fire(evt)
      return evt == long_start or evt == long_repeat
    end
    if down then
      pcall(function()
        key.on(down, function(evt)
          if fire(evt) then toggle_chat() end
        end)
      end)
    end
    if left then
      pcall(function()
        key.on(left, function(evt)
          if long_fire(evt) then
            self.ui:set_view_mode("default")
          elseif evt == short then
            start_listening(State.LISTEN_MANUAL)
          end
        end)
      end)
    end
    if right then
      pcall(function()
        key.on(right, function(evt)
          if long_fire(evt) then
            self.ui:set_view_mode("wechat")
          elseif evt == short then
            stop_listening()
          end
        end)
      end)
    end
  end

  local function unbind_keys()
    if not key or not key.off then
      return
    end
    pcall(function() key.off(key.DOWN or rawget(_G, "KEY_DOWN")) end)
    pcall(function() key.off(key.LEFT or rawget(_G, "KEY_LEFT")) end)
    pcall(function() key.off(key.RIGHT or rawget(_G, "KEY_RIGHT")) end)
  end

  local function start_timer()
    if not tmr or not tmr.create then
      return
    end
    self.timer = tmr.create()
    self.timer:alarm(700, tmr.ALARM_AUTO, function()
      if app and app.exiting and app.exiting() then
        self.stop("app.exiting")
        return
      end
      refresh_metrics()
      if self.ui then
        self.ui:update_status_bar(false)
      end
    end)
  end

  function self:start()
    self.stopped = false
    self.ui:setup()
    self.audio = Audio.new(cfg)
    self.protocol = Protocol.new(cfg)
    self.mcp = Mcp.new(cfg, function(payload)
      return self.protocol:send_mcp_message(payload)
    end, function()
      if self.stop then self.stop("mcp app switch") end
    end)
    bind_audio()
    bind_protocol()
    self.state:on_change(on_state_changed)

    set_state(State.STARTING)
    if not self.audio:load_modules() then
      alert("错误", self.audio.last_error, "circle_xmark")
      set_state(State.FATAL_ERROR)
      return false
    end

    set_state(State.ACTIVATING)
    bind_keys()
    start_timer()
    local activation_started = start_activation()
    if not activation_started then
      self.protocol:start()
      self.ui:show_notification("xiaozhi lua port", 1200)
      set_state(State.IDLE)
    end
    refresh_metrics()
    return true
  end

  local function do_stop(reason)
    if self.stopped then
      return
    end
    self.stopped = true
    if self.timer then
      pcall(function() self.timer:stop() end)
      pcall(function() self.timer:unregister() end)
      self.timer = nil
    end
    if self.wake_open_timer then
      pcall(function() self.wake_open_timer:stop() end)
      pcall(function() self.wake_open_timer:unregister() end)
      self.wake_open_timer = nil
    end
    unbind_keys()
    if self.protocol then
      self.protocol:stop()
    end
    if self.activation then
      self.activation:stop()
      self.activation = nil
    end
    if self.audio then
      self.audio:stop()
    end
    if self.ui then
      self.ui:stop()
    end
    print("[xiaozhi] stop", reason or "")
  end

  self.stop = do_stop

  self.toggle_chat = toggle_chat
  self.start_listening = start_listening
  self.stop_listening = stop_listening
  self.wake_word_invoke = wake_word_invoke

  return self
end

return M
