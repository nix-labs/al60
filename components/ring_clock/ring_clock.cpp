#include "ring_clock.h"

// lwIP SNTP daemon control — allows truly stopping NTP syncs in manual mode.
// on_time_sync is notification-only; lwIP has already applied settimeofday()
// before the callback fires, so the only reliable gate is sntp_stop().
#ifdef USE_ESP32
#include "esp_sntp.h"
#elif defined(USE_ESP8266)
#include "sntp/sntp.h"
#endif

namespace esphome {
namespace ring_clock {

  const char *TAG = "ring_clock.component";

  // --- Helpers ---

  // IRAM_ATTR: keep in on-chip SRAM so an RMT DMA refill cycle cannot stall this path.
  static IRAM_ATTR Color get_cv_color(const light::LightColorValues& cv) {
      // Use standard as_rgb to capture all compounded brightness factors.
      // Gamma correction is disabled on the source lights to keep these values linear.
      float r, g, b;
      cv.as_rgb(&r, &g, &b);
      return Color((uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(b * 255.0f));
  }

  // --- Lifecycle ---

  void RingClock::setup() {
    // Sort color gradient points once at boot so per-frame calls to
    // get_temp_color() / get_humid_color() don't need to sort on every call.
    auto cmp = [](const ColorPoint &a, const ColorPoint &b) { return a.value < b.value; };
    std::sort(_temp_color_points.begin(), _temp_color_points.end(), cmp);
    std::sort(_humid_color_points.begin(), _humid_color_points.end(), cmp);
  }

  void RingClock::loop() {
    // Check if time has become valid (synced via NTP or RTC)
    if (!_has_time && _time->now().is_valid()) {
      _has_time = true;
      on_ready();
      // Do not return early — fall through so the alarm check runs this tick too
    }

    // Handle Alarm Timeout
    if (_alarm_active) {
      if (!_alarm_dispatched) {
        this->on_alarm_triggered();
        _alarm_dispatched = true;
      }
      // Auto-dismiss visual alarm after configured duration
      if (millis() - _alarm_triggered_ms > ALARM_VISUAL_DURATION_MS) {
        _alarm_active = false;
      }
    }

    // --- Smooth brightness stepping ---
    if (_brightness_target >= 0.0f && _brightness_current >= 0.0f
        && _clock_lights != nullptr) {
      float diff = _brightness_target - _brightness_current;
      if (fabsf(diff) > 0.002f) {
        uint32_t now_ms = millis();
        uint32_t elapsed_ms = now_ms - _brightness_step_ms;
        if (elapsed_ms >= BRIGHTNESS_STEP_MS) {
          float step = BRIGHTNESS_SPEED * (elapsed_ms / 1000.0f);
          float delta = std::min(step, fabsf(diff));
          _brightness_current += (diff > 0.0f ? 1.0f : -1.0f) * delta;
          _brightness_current = std::max(0.0f, std::min(1.0f, _brightness_current));
          _brightness_step_ms = now_ms;
          // Push updated brightness to the light state.
          // transition_length=0 avoids ESPHome's transition system entirely;
          // current_values is updated immediately for the next effect render.
          auto call = _clock_lights->turn_on();
          call.set_brightness(_brightness_current);
          call.set_transition_length(0);
          call.perform();
        }
      }
    }
  }

  // --- Event & Callback Handlers ---

  void RingClock::on_ready() {
    _state = state::time;
    ESP_LOGD(TAG, "RingClock Ready: Time is valid.");
    this->_on_ready_callback_.call();
  }

  void RingClock::add_on_ready_callback(std::function<void()> callback) {
    this->_on_ready_callback_.add(std::move(callback));
  }

  void RingClock::add_on_timer_finished_callback(std::function<void()> callback) { this->_on_timer_finished_callback_.add(std::move(callback)); }
  void RingClock::on_timer_finished() {
    if (this->_sound_enabled_switch == nullptr || this->_sound_enabled_switch->state)
      this->_on_timer_finished_callback_.call();
  }

  void RingClock::add_on_timer_started_callback(std::function<void()> callback) { this->_on_timer_started_callback_.add(std::move(callback)); }
  void RingClock::on_timer_started() {
    if (this->_sound_enabled_switch == nullptr || this->_sound_enabled_switch->state)
      this->_on_timer_started_callback_.call();
  }

  void RingClock::add_on_timer_stopped_callback(std::function<void()> callback) { this->_on_timer_stopped_callback_.add(std::move(callback)); }
  void RingClock::on_timer_stopped() {
    if (this->_sound_enabled_switch == nullptr || this->_sound_enabled_switch->state)
      this->_on_timer_stopped_callback_.call();
  }

  void RingClock::add_on_stopwatch_minute_callback(std::function<void()> callback) { this->_on_stopwatch_minute_callback_.add(std::move(callback)); }
  void RingClock::on_stopwatch_minute() {
    if (this->_sound_enabled_switch == nullptr || this->_sound_enabled_switch->state)
      this->_on_stopwatch_minute_callback_.call();
  }

  void RingClock::add_on_stopwatch_started_callback(std::function<void()> callback) { this->_on_stopwatch_started_callback_.add(std::move(callback)); }
  void RingClock::on_stopwatch_started() {
    if (this->_sound_enabled_switch == nullptr || this->_sound_enabled_switch->state)
      this->_on_stopwatch_started_callback_.call();
  }

  void RingClock::add_on_stopwatch_paused_callback(std::function<void()> callback) { this->_on_stopwatch_paused_callback_.add(std::move(callback)); }
  void RingClock::on_stopwatch_paused() {
    if (this->_sound_enabled_switch == nullptr || this->_sound_enabled_switch->state)
      this->_on_stopwatch_paused_callback_.call();
  }

  void RingClock::add_on_stopwatch_reset_callback(std::function<void()> callback) { this->_on_stopwatch_reset_callback_.add(std::move(callback)); }
  void RingClock::on_stopwatch_reset() {
    if (this->_sound_enabled_switch == nullptr || this->_sound_enabled_switch->state)
      this->_on_stopwatch_reset_callback_.call();
  }

  void RingClock::add_on_alarm_triggered_callback(std::function<void()> callback) { this->_on_alarm_triggered_callback_.add(std::move(callback)); }
  void RingClock::on_alarm_triggered() { this->_on_alarm_triggered_callback_.call(); }

  void RingClock::start_alarm() {
    _alarm_triggered_ms = millis();
    _alarm_dispatched = false;
    _alarm_active = true;
  }

  // --- Logic Control ---

  void RingClock::start_timer(int hours, int minutes, int seconds) {
    if (hours > 12) hours = 12;
    if (hours == 12 && minutes > 59) minutes = 59;
    if (hours == 12 && minutes == 59 && seconds > 59) seconds = 59;

    _timer_duration_ms = (hours * 3600 + minutes * 60 + seconds) * 1000;
    _timer_target_ms = millis() + _timer_duration_ms;
    _timer_active = true;
    _timer_finished_ms = 0;
    _timer_finishing_dispatched = false;
    _state = state::timer;
    this->on_timer_started();
  }

  void RingClock::stop_timer() {
    _timer_active = false;
    _state = state::time;
    this->on_timer_stopped();
  }

  void RingClock::start_stopwatch() {
    if (!_stopwatch_active) {
      _stopwatch_start_ms = millis() - _stopwatch_paused_ms;
      _stopwatch_active = true;
      _stopwatch_last_minute = -1;
      this->on_stopwatch_started();
    }
    _state = state::stopwatch;
  }

  void RingClock::pause_stopwatch() {
    if (_stopwatch_active) {
      _stopwatch_paused_ms = millis() - _stopwatch_start_ms;
      _stopwatch_active = false;
      this->on_stopwatch_paused();
    }
  }

  void RingClock::stop_stopwatch() {
    _stopwatch_active = false;
    _stopwatch_paused_ms = 0;
    _stopwatch_last_minute = -1;
    _state = state::time;
    this->on_stopwatch_reset();
  }

  void RingClock::reset_stopwatch() {
    _stopwatch_start_ms = millis();
    _stopwatch_paused_ms = 0;
    _stopwatch_last_minute = -1;
    this->on_stopwatch_reset();
  }

  state RingClock::get_state() { return _state; }
  void RingClock::set_state(state s) { _state = s; }

  // --- Configuration Setters ---

  void RingClock::set_time(time::RealTimeClock *time) { _time = time; }
  void RingClock::set_hour_hand_color_state(light::LightState* s)   { this->hour_hand_color   = s; }
  void RingClock::set_minute_hand_color_state(light::LightState* s) { this->minute_hand_color = s; }
  void RingClock::set_second_hand_color_state(light::LightState* s) { this->second_hand_color = s; }
  void RingClock::set_marker_color_state(light::LightState* s)      { this->marker_color      = s; }
  void RingClock::set_notification_color_state(light::LightState* s){ this->notification_color = s; }
  void RingClock::set_clock_addressable_lights(light::LightState *it){ this->_clock_lights = it; }
  void RingClock::set_blank_leds(std::vector<int> leds) { this->_blanked_leds = leds; }
  float RingClock::get_interference_factor() { return this->_interference_factor; }

  // --- Time Management ---

  // Proleptic Gregorian calendar fields -> Unix UTC epoch.
  // Pure arithmetic, no mktime(), no TZ environment side-effects.
  time_t RingClock::fields_to_epoch(int year, int month, int day,
                                    int hour, int minute, int second) {
    int y = year, m = month;
    if (m <= 2) { y--; m += 12; }
    long days = 365L * y + y / 4 - y / 100 + y / 400
              + (153 * (m - 3) + 2) / 5 + day - 719469;
    return (time_t)(days * 86400L + (long)hour * 3600L + (long)minute * 60L + second);
  }

  void RingClock::set_sntp_enabled(bool enabled) {
    _sntp_enabled = enabled;
    if (!_network_ready) {
      // TCP/IP stack not up yet — just record intent; set_network_ready()
      // will apply the actual daemon state once WiFi connects.
      ESP_LOGI(TAG, "SNTP %s (pending — network not ready).",
               enabled ? "enabled" : "disabled (manual mode)");
      return;
    }
    if (enabled) {
      if (!esp_sntp_enabled()) {
        esp_sntp_init();
      }
      ESP_LOGI(TAG, "SNTP daemon started (auto mode).");
    } else {
      if (esp_sntp_enabled()) {
        esp_sntp_stop();
      }
      ESP_LOGI(TAG, "SNTP daemon stopped (manual mode).");
    }
  }

  void RingClock::set_network_ready() {
    _network_ready = true;
    if (!_sntp_enabled) {
      // Apply pending manual-mode stop
      if (esp_sntp_enabled()) {
        esp_sntp_stop();
      }
      ESP_LOGI(TAG, "Network ready: SNTP daemon stopped (manual mode).");
    } else {
      // Always restart the daemon when WiFi comes up.
      // ESPHome calls esp_sntp_init() at setup() time before WiFi is
      // available, so no actual NTP request fires. Restarting here
      // guarantees an immediate sync on every WiFi connect (including
      // first boot after factory reset).
      if (esp_sntp_enabled()) {
        esp_sntp_stop();
      }
      esp_sntp_init();
      ESP_LOGI(TAG, "Network ready: SNTP daemon restarted — syncing now.");
    }
  }

  void RingClock::set_target_brightness(float target) {
    _brightness_target = target;
    if (target < 0.0f) {
      // Manual mode: disengage stepper. Leave _brightness_current as-is so
      // the ring stays at whatever level it was; the HA slider now controls it.
      return;
    }
    if (_brightness_current < 0.0f && _clock_lights != nullptr) {
      // First initialisation — sync from whatever ESPHome has and snap immediately.
      _brightness_current = _clock_lights->current_values.get_brightness();
      if (_brightness_current <= 0.0f) _brightness_current = target;
      _brightness_step_ms = millis();
      // Apply the snapped value instantly (no step needed at boot).
      auto call = _clock_lights->turn_on();
      call.set_brightness(_brightness_current);
      call.set_transition_length(0);
      call.perform();
    }
    // If target has changed and we were already running:
    // loop() will start stepping toward the new target automatically.
  }

  bool RingClock::get_sntp_enabled() const { return _sntp_enabled; }

  void RingClock::apply_sntp_sync(time_t utc_epoch) {
    if (!_sntp_enabled) {
      ESP_LOGI(TAG, "SNTP sync arrived but ignored (manual mode active).");
      return;
    }
    struct timeval tv = { .tv_sec = utc_epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGW(TAG, "SNTP sync applied — UTC epoch: %ld", (long)utc_epoch);
  }

  void RingClock::set_manual_time(int year, int month, int day,
                                   int hour, int minute, int second) {
    // Derive the active timezone offset.
    time_t utc_now = _time->timestamp_now();
    auto now = _time->now();
    time_t local_as_utc = fields_to_epoch(
        now.year, now.month, now.day_of_month,
        now.hour, now.minute, now.second);
    int32_t tz_offset = (int32_t)(local_as_utc - utc_now);

    // Convert the user-entered local datetime to UTC and apply
    time_t entered_utc = fields_to_epoch(year, month, day, hour, minute, second);
    time_t utc_epoch   = entered_utc - tz_offset;

    struct timeval tv = { .tv_sec = utc_epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // Switch to manual mode — block future automatic SNTP overwrites
    set_sntp_enabled(false);
    ESP_LOGI(TAG, "Manual time set — UTC epoch: %ld (tz_offset: %ds)",
             (long)utc_epoch, tz_offset);
  }

  void RingClock::increment_hour() {
    time_t utc_now = _time->timestamp_now();
    auto now = _time->now();
    time_t local_as_utc = fields_to_epoch(
        now.year, now.month, now.day_of_month,
        now.hour, now.minute, now.second);
    int32_t tz_offset = (int32_t)(local_as_utc - utc_now);

    // Increment hour with wrapping (0-23)
    now.hour = (now.hour + 1) % 24;
    now.second = 0;

    time_t entered_utc = fields_to_epoch(now.year, now.month, now.day_of_month, now.hour, now.minute, now.second);
    time_t utc_epoch   = entered_utc - tz_offset;

    struct timeval tv = { .tv_sec = utc_epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    set_sntp_enabled(false);
    ESP_LOGI(TAG, "Hour incremented (isolated, wrapped).");
  }

  void RingClock::increment_minute() {
    time_t utc_now = _time->timestamp_now();
    auto now = _time->now();
    time_t local_as_utc = fields_to_epoch(
        now.year, now.month, now.day_of_month,
        now.hour, now.minute, now.second);
    int32_t tz_offset = (int32_t)(local_as_utc - utc_now);

    // Increment minute with wrapping (0-59)
    now.minute = (now.minute + 1) % 60;
    now.second = 0;

    time_t entered_utc = fields_to_epoch(now.year, now.month, now.day_of_month, now.hour, now.minute, now.second);
    time_t utc_epoch   = entered_utc - tz_offset;

    struct timeval tv = { .tv_sec = utc_epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    set_sntp_enabled(false);
    ESP_LOGI(TAG, "Minute incremented (isolated, wrapped).");
  }

  bool RingClock::should_sweep() {
    return this->_hour_sweep_switch != nullptr && this->_hour_sweep_switch->state;
  }

  // --- Ring Helpers ---

  void RingClock::clear_R1(light::AddressableLight & it) {
    for (int i = 0; i < R1_NUM_LEDS; i++) it[i] = Color(0, 0, 0);
  }

  void RingClock::clear_R2(light::AddressableLight & it) {
    for (int i = R1_NUM_LEDS; i < TOTAL_LEDS; i++) it[i] = Color(0, 0, 0);
  }

  // Resolves the display color for one clock hand.
  // Priority: Rainbow effect → sensor effects → custom CV color → default.
  // is_minute_complement shifts the Rainbow hue by 180° so minute and hour hands
  // sit on opposite sides of the colour wheel.
  Color RingClock::resolve_hand_color(light::LightState* ls, Color default_color,
                                      const esphome::ESPTime& now,
                                      bool is_minute_complement) {
    if (ls == nullptr) return default_color;

    std::string eff = ls->get_effect_name().str();

    if (eff == "Rainbow") {
      float cycle = ((now.hour % 12) * 3600 + now.minute * 60 + now.second) / 43200.0f;
      float hue = fmod(cycle * 360.0f + (is_minute_complement ? 180.0f : 0.0f), 360.0f);
      float r, g, b;
      esphome::hsv_to_rgb(hue, 1.0f, 1.0f, r, g, b);
      float br = ls->current_values.get_brightness();
      return Color((uint8_t)(r * 255.0f * br), (uint8_t)(g * 255.0f * br), (uint8_t)(b * 255.0f * br));
    }

    if (!ls->current_values.get_state()) return default_color;

    float br = ls->current_values.get_brightness();
    if (eff == "Temperature Color") {
      Color c = get_temp_color(_temp_sensor ? _temp_sensor->state : 20.0f);
      return Color((uint8_t)(c.r * br), (uint8_t)(c.g * br), (uint8_t)(c.b * br));
    }
    if (eff == "Humidity Color") {
      Color c = get_humid_color(_humidity_sensor ? _humidity_sensor->state : 50.0f);
      return Color((uint8_t)(c.r * br), (uint8_t)(c.g * br), (uint8_t)(c.b * br));
    }
    return get_cv_color(ls->current_values);
  }

  // Draws the hour hand on R2.  When sweep is enabled, the hand interpolates
  // smoothly between adjacent marker LEDs using a sqrt blend.
  void RingClock::draw_hour_hand(light::AddressableLight & it, Color hc, const esphome::ESPTime& now) {
    if (this->should_sweep()) {
      float precise_hour = (now.hour % 12) + (now.minute / 60.0f) + (now.second / 3600.0f);
      float pos = precise_hour * 4.0f;
      int idx1 = (int)floorf(pos) % R2_NUM_LEDS;
      int idx2 = (idx1 + 1) % R2_NUM_LEDS;
      float frac = pos - floorf(pos);
      float i1 = sqrtf(1.0f - frac);
      float i2 = sqrtf(frac);
      auto bg1 = it[R1_NUM_LEDS + idx1].get();
      it[R1_NUM_LEDS + idx1] = Color(
        (uint8_t)(hc.r * i1 + bg1.r * (1.0f - i1)),
        (uint8_t)(hc.g * i1 + bg1.g * (1.0f - i1)),
        (uint8_t)(hc.b * i1 + bg1.b * (1.0f - i1)));
      if (frac > 0.0f) {
        auto bg2 = it[R1_NUM_LEDS + idx2].get();
        it[R1_NUM_LEDS + idx2] = Color(
          (uint8_t)(hc.r * i2 + bg2.r * (1.0f - i2)),
          (uint8_t)(hc.g * i2 + bg2.g * (1.0f - i2)),
          (uint8_t)(hc.b * i2 + bg2.b * (1.0f - i2)));
      }
    } else {
      it[R1_NUM_LEDS + ((now.hour % 12) * 4)] = hc;
    }
  }

  // --- Rendering Dispatch ---

  IRAM_ATTR void RingClock::addressable_lights_lambdacall(light::AddressableLight & it) {
    // Skip rendering if nothing has changed.
    const bool rain_h = hour_hand_color   && hour_hand_color->get_effect_name()   == "Rainbow";
    const bool rain_m = minute_hand_color && minute_hand_color->get_effect_name() == "Rainbow";
    const bool rain_s = second_hand_color && second_hand_color->get_effect_name() == "Rainbow";
    // True when _brightness_current is actively moving toward _brightness_target.
    // Bypasses the dirty-bit cache so re-renders happen at the effect's update_interval.
    const bool brightness_changing = (_brightness_target >= 0.0f)
                                  && (_brightness_current >= 0.0f)
                                  && (fabsf(_brightness_current - _brightness_target) > 0.002f);
    const bool is_dynamic =
        (_state == state::stopwatch)                          // sub-second elapsed counter
     || (_state == state::timer && _timer_active)             // countdown + pulse animation
     || (_alarm_active)                                       // sinf(millis()) pulse overlay
     || (rain_h || rain_m || rain_s)                          // HSV cycle changes every frame
     || (_state == state::time_fade)                          // millis()-driven fade progress
     || (_state == state::time_tail)                          // moving 15-LED tail
     || brightness_changing;                                  // smooth brightness transition

    // Fetch time once here; pass it into sub-renderers to avoid a second RTC read.
    esphome::ESPTime now = this->_time->now();

    if (!is_dynamic
        && now.second == _cache_s
        && now.minute == _cache_m
        && now.hour   == _cache_h
        && _state     == _cache_mode) {
      return;  // Nothing changed — skip RMT write entirely (~98% of frames)
    }

    // Update cache with the values we are about to render.
    _cache_s    = now.second;
    _cache_m    = now.minute;
    _cache_h    = now.hour;
    _cache_mode = _state;

    switch (_state) {
      case state::time:
      case state::alarm:
        render_time(it, false, now);
        break;
      case state::time_fade:
        render_time(it, true, now);
        break;
      case state::time_tail:
        render_tail(it, now);
        break;
      case state::timer:
        render_timer(it);
        break;
      case state::stopwatch:
        render_stopwatch(it);
        break;
      case state::sensors_bars:
        render_sensors_bars(it);
        break;
      case state::sensors_temp_bar:
        render_sensors_bar_individual(it, true);
        break;
      case state::sensors_humid_bar:
        render_sensors_bar_individual(it, false);
        break;
      case state::sensors_temp_glow:
        render_sensors_temp_glow(it);
        break;
      case state::sensors_humid_glow:
        render_sensors_humid_glow(it);
        break;
      case state::sensors_dual_glow:
        render_sensors_dual_glow(it);
        break;
      case state::sensors_ticks:
        render_sensors_ticks(it);
        break;
      case state::sensors_temp_tick:
        render_sensors_tick_individual(it, true);
        break;
      case state::sensors_humid_tick:
        render_sensors_tick_individual(it, false);
        break;
    }

    // Overlay: Alarm animation (pulsing ring) — drawn on top of whatever state is active
    if (_alarm_active) {
      render_alarm(it);
    }

    // Overlay: Blank LEDs (hardware masking for obstruction/wiring)
    if (!_blanked_leds.empty()) {
      for (int idx : _blanked_leds) {
        if (idx >= 0 && idx < TOTAL_LEDS)
          it[idx] = Color(0, 0, 0);
      }
    }

    // Interference estimate for the ambient light sensor.
    // Uses the two LEDs physically closest to the sensor on the PCB.
    {
      auto c_r1 = it[SENSOR_ADJACENT_LED_R1].get();
      auto c_r2 = it[R1_NUM_LEDS + SENSOR_ADJACENT_LED_R2].get();
      float b_r1 = (c_r1.r + c_r1.g + c_r1.b) / 3.0f;
      float b_r2 = (c_r2.r + c_r2.g + c_r2.b) / 3.0f;
      this->_interference_factor = (b_r1 + b_r2) / (2.0f * 255.0f);
    }
  }

  void RingClock::draw_markers(light::AddressableLight & it) {
    bool sensor_effect_active = false;

    if (this->notification_color != nullptr) {
      std::string effect = this->notification_color->get_effect_name();

      if (effect.find("Sensors:") != std::string::npos) {
        if      (effect == "Sensors: Dual Bars")        { render_sensors_bars(it);               sensor_effect_active = true; }
        else if (effect == "Sensors: Temperature Bar")  { render_sensors_bar_individual(it, true);  sensor_effect_active = true; }
        else if (effect == "Sensors: Humidity Bar")     { render_sensors_bar_individual(it, false); sensor_effect_active = true; }
        else if (effect == "Sensors: Temperature Glow") { render_sensors_temp_glow(it);          sensor_effect_active = true; }
        else if (effect == "Sensors: Humidity Glow")    { render_sensors_humid_glow(it);         sensor_effect_active = true; }
        else if (effect == "Sensors: Dual Ticks")       { render_sensors_ticks(it);              sensor_effect_active = true; }
        else if (effect == "Sensors: Temperature Tick") { render_sensors_tick_individual(it, true);  sensor_effect_active = true; }
        else if (effect == "Sensors: Humidity Tick")    { render_sensors_tick_individual(it, false); sensor_effect_active = true; }
        else if (effect == "Sensors: Dual Glow")        { render_sensors_dual_glow(it);          sensor_effect_active = true; }
      }
    }

    if (!sensor_effect_active) {
      // Draw notification background color if enabled
      if (this->notification_color != nullptr && this->notification_color->current_values.get_state()) {
        auto cv = this->notification_color->current_values;
        Color bg = get_cv_color(cv);
        // Minimum visibility: ensure non-zero channels don't disappear at low brightness
        if (cv.get_red()   > 0 && bg.r < 10) bg.r = 10;
        if (cv.get_green() > 0 && bg.g < 10) bg.g = 10;
        if (cv.get_blue()  > 0 && bg.b < 10) bg.b = 10;

        for (int i = R1_NUM_LEDS; i < TOTAL_LEDS; i++) {
          bool is_marker = ((i - R1_NUM_LEDS) % 4 == 0);
          if (this->marker_color != nullptr && this->marker_color->current_values.get_state()) {
            if (!is_marker) it[i] = bg;
          } else {
            it[i] = bg;
          }
        }
      }
    }

    // Draw hour markers on R2
    if (this->marker_color != nullptr && this->marker_color->current_values.get_state()) {
      Color mc = _default_marker_color;
      std::string eff = this->marker_color->get_effect_name();
      float br = this->marker_color->current_values.get_brightness();

      if (eff == "Temperature Color") {
        Color c = get_temp_color(_temp_sensor ? _temp_sensor->state : 20.0f);
        mc = Color((uint8_t)(c.r * br), (uint8_t)(c.g * br), (uint8_t)(c.b * br));
      } else if (eff == "Humidity Color") {
        Color c = get_humid_color(_humidity_sensor ? _humidity_sensor->state : 50.0f);
        mc = Color((uint8_t)(c.r * br), (uint8_t)(c.g * br), (uint8_t)(c.b * br));
      } else {
        mc = get_cv_color(this->marker_color->current_values);
      }

      // Iterate only the 12 marker positions directly (stride 4) rather than
      // all 48 R2 LEDs with a per-iteration modulo check.
      for (int m = 0; m < 12; m++) {
        int i = R1_NUM_LEDS + (m * 4);
        if (_marker_highlight_mode == MarkerHighlightMode::NONE) {
          it[i] = mc;
        } else {
          bool highlight = (_marker_highlight_mode == MarkerHighlightMode::TWELVE_ONLY)
              ? (m == 0)
              : (m == 0 || m == 3 || m == 6 || m == 9);
          float contrast = highlight ? 1.0f : 0.35f;
          it[i] = Color((uint8_t)(mc.r * contrast), (uint8_t)(mc.g * contrast), (uint8_t)(mc.b * contrast));
        }
      }
    }
  }

  // IRAM_ATTR: keep render functions in SRAM for consistent ISR timing.
  IRAM_ATTR void RingClock::render_time(light::AddressableLight & it, bool fade, const esphome::ESPTime & now) {
    clear_R1(it);
    clear_R2(it);
    draw_markers(it);

    // `now` is pre-fetched by the caller — no second RTC read needed.
    Color hc = resolve_hand_color(hour_hand_color,   _default_hour_color,   now);
    Color mc = resolve_hand_color(minute_hand_color,  _default_minute_color, now, true);
    Color sc = resolve_hand_color(second_hand_color,  _default_second_color, now);

    // Override second Rainbow: use a drifting phase unrelated to clock position
    // so it doesn't always appear red at 12 o'clock
    if (second_hand_color != nullptr && second_hand_color->get_effect_name() == "Rainbow") {
      float cycle = fmod(millis() / 47000.0f, 1.0f);
      float r, g, b;
      esphome::hsv_to_rgb(cycle * 360.0f, 1.0f, 1.0f, r, g, b);
      float br = second_hand_color->current_values.get_brightness();
      sc = Color((uint8_t)(r * 255.0f * br), (uint8_t)(g * 255.0f * br), (uint8_t)(b * 255.0f * br));
    }

    draw_hour_hand(it, hc, now);

    if (second_hand_color != nullptr && second_hand_color->current_values.get_state()) {
      if (fade) {
        if (this->last_second != now.second) {
          this->last_second = now.second;
          this->last_second_timestamp = millis();
        }
        float progress = std::min((millis() - this->last_second_timestamp) / 1000.0f, 1.0f);
        draw_fade(it, now.second + progress, sc);
      } else {
        it[now.second] = sc;
      }
    }

    it[now.minute] = mc;
  }

  IRAM_ATTR void RingClock::render_tail(light::AddressableLight & it, const esphome::ESPTime & now) {
    clear_R1(it);
    clear_R2(it);
    draw_markers(it);

    // `now` is pre-fetched by the caller — no second RTC read needed.
    Color hc = resolve_hand_color(hour_hand_color,  _default_hour_color,   now);
    Color mc = resolve_hand_color(minute_hand_color, _default_minute_color, now, true);

    draw_hour_hand(it, hc, now);

    // Seconds tail
    if (second_hand_color != nullptr && second_hand_color->current_values.get_state()) {
      if (this->last_second != now.second) {
        this->last_second = now.second;
        this->last_second_timestamp = millis();
      }
      float progress = std::min((millis() - this->last_second_timestamp) / 1000.0f, 1.0f);
      float precise_pos = now.second + progress;

      std::string s_eff = second_hand_color->get_effect_name().str();
      if (s_eff == "Rainbow") {
        float cycle_offset = fmod(millis() / 47000.0f, 1.0f) * 360.0f;
        float br = second_hand_color->current_values.get_brightness();
        int tail_length = 15;
        for (int i = 0; i < 60; i++) {
          float dist = precise_pos - i;
          if (dist < 0) dist += 60.0f;
          if (dist >= 0 && dist < tail_length) {
            float intensity = (1.0f - (dist / (float)tail_length));
            intensity *= intensity;
            float hue = fmod((i * (360.0f / 60.0f)) + cycle_offset, 360.0f);
            float r, g, b;
            esphome::hsv_to_rgb(hue, 1.0f, 1.0f, r, g, b);
            it[i] = Color(
              (uint8_t)(r * 255.0f * intensity * br),
              (uint8_t)(g * 255.0f * intensity * br),
              (uint8_t)(b * 255.0f * intensity * br));
          }
        }
      } else {
        Color sc = resolve_hand_color(second_hand_color, _default_second_color, now);
        draw_tail(it, precise_pos, sc);
      }
    }

    it[now.minute] = mc;
  }

  // --- Private Helpers ---

  void RingClock::draw_tail(light::AddressableLight & it, float precise_pos, Color color) {
    int tail_length = 15;
    for (int i = 0; i < 60; i++) {
      float dist = precise_pos - i;
      if (dist < 0) dist += 60.0f;
      if (dist >= 0 && dist < tail_length) {
        float intensity = 1.0f - (dist / (float)tail_length);
        intensity *= intensity;
        it[i] = Color(
          (uint8_t)(color.r * intensity),
          (uint8_t)(color.g * intensity),
          (uint8_t)(color.b * intensity));
      }
    }
  }

  void RingClock::draw_fade(light::AddressableLight &it, float precise_pos, Color color) {
    for (int i = 0; i < 60; i++) {
      float dist = fabsf(i - precise_pos);
      if (dist > 30.0f) dist = 60.0f - dist;
      if (dist < 1.5f) {
        float scale = 1.0f - (dist / 1.5f);
        it[i] = Color(
          (uint8_t)(color.r * scale),
          (uint8_t)(color.g * scale),
          (uint8_t)(color.b * scale));
      }
    }
  }

  // --- Sensor Color Lookups ---

  Color RingClock::get_temp_color(float t) {
    if (_temp_color_points.empty()) {
      // Fallback hardcoded gradient
      if (t <= -10.0f) return Color(26, 22, 73);
      struct { float lo, hi; Color c0, c1; } segs[] = {
        { -10, 0,  Color(26,22,73),    Color(18,94,131)  },
        {   0, 10, Color(18,94,131),   Color(60,157,116) },
        {  10, 20, Color(60,157,116),  Color(207,216,113)},
        {  20, 30, Color(207,216,113), Color(223,158,60) },
        {  30, 40, Color(223,158,60),  Color(193,59,44)  },
        {  40, 50, Color(193,59,44),   Color(136,26,23)  },
      };
      for (auto& s : segs) {
        if (t <= s.hi) {
          float p = (t - s.lo) / (s.hi - s.lo);
          return Color(
            (uint8_t)(s.c0.r + p * (s.c1.r - s.c0.r)),
            (uint8_t)(s.c0.g + p * (s.c1.g - s.c0.g)),
            (uint8_t)(s.c0.b + p * (s.c1.b - s.c0.b)));
        }
      }
      return Color(136, 26, 23);
    }

    // Points are pre-sorted in setup()
    if (t <= _temp_color_points.front().value) return _temp_color_points.front().color;
    if (t >= _temp_color_points.back().value)  return _temp_color_points.back().color;
    for (size_t i = 0; i < _temp_color_points.size() - 1; i++) {
      if (t >= _temp_color_points[i].value && t <= _temp_color_points[i+1].value) {
        float p = (t - _temp_color_points[i].value) /
                  (_temp_color_points[i+1].value - _temp_color_points[i].value);
        return Color(
          (uint8_t)(_temp_color_points[i].color.r + p * (_temp_color_points[i+1].color.r - _temp_color_points[i].color.r)),
          (uint8_t)(_temp_color_points[i].color.g + p * (_temp_color_points[i+1].color.g - _temp_color_points[i].color.g)),
          (uint8_t)(_temp_color_points[i].color.b + p * (_temp_color_points[i+1].color.b - _temp_color_points[i].color.b)));
      }
    }
    return Color(0, 0, 0);
  }

  Color RingClock::get_humid_color(float h) {
    if (_humid_color_points.empty()) {
      struct { float lo, hi; Color c0, c1; } segs[] = {
        {  0, 30, Color(190,155,47), Color(160,195,27) },
        { 30, 70, Color(160,195,27), Color(60,215,127) },
        { 70,100, Color(60,215,127), Color(20,95,227)  },
      };
      if (h <= 0)   return segs[0].c0;
      if (h >= 100) return segs[2].c1;
      for (auto& s : segs) {
        if (h <= s.hi) {
          float p = (h - s.lo) / (s.hi - s.lo);
          return Color(
            (uint8_t)(s.c0.r + p * (s.c1.r - s.c0.r)),
            (uint8_t)(s.c0.g + p * (s.c1.g - s.c0.g)),
            (uint8_t)(s.c0.b + p * (s.c1.b - s.c0.b)));
        }
      }
      return Color(20, 95, 227);
    }

    if (h <= _humid_color_points.front().value) return _humid_color_points.front().color;
    if (h >= _humid_color_points.back().value)  return _humid_color_points.back().color;
    for (size_t i = 0; i < _humid_color_points.size() - 1; i++) {
      if (h >= _humid_color_points[i].value && h <= _humid_color_points[i+1].value) {
        float p = (h - _humid_color_points[i].value) /
                  (_humid_color_points[i+1].value - _humid_color_points[i].value);
        return Color(
          (uint8_t)(_humid_color_points[i].color.r + p * (_humid_color_points[i+1].color.r - _humid_color_points[i].color.r)),
          (uint8_t)(_humid_color_points[i].color.g + p * (_humid_color_points[i+1].color.g - _humid_color_points[i].color.g)),
          (uint8_t)(_humid_color_points[i].color.b + p * (_humid_color_points[i+1].color.b - _humid_color_points[i].color.b)));
      }
    }
    return Color(0, 0, 0);
  }

  // --- Sensor Renderers ---

  void RingClock::render_sensors_bars(light::AddressableLight & it) {
    float temp  = _temp_sensor      ? _temp_sensor->state      : 20.0f;
    float humid = _humidity_sensor  ? _humidity_sensor->state  : 50.0f;
    Color nc = get_cv_color(this->notification_color->current_values);

    // Temperature bar — right side of R2 (hours 5 down to 0), 18 LEDs
    float t_p = (temp + 10.0f) / 60.0f;
    int t_leds = (int)std::max(0.0f, std::min(18.0f, t_p * 18.0f));
    int count = 0;
    for (int h = 5; h >= 0; h--) {
      for (int s = 3; s >= 1; s--) {
        if (count < t_leds) {
          Color c = get_temp_color(-10.0f + (count * (60.0f / 18.0f)));
          it[R1_NUM_LEDS + (h * 4) + s] = Color(
            (uint8_t)(c.r * nc.r / 255.0f),
            (uint8_t)(c.g * nc.g / 255.0f),
            (uint8_t)(c.b * nc.b / 255.0f));
        }
        count++;
      }
    }

    // Humidity bar — left side of R2 (hours 6 up to 11), 18 LEDs
    float h_p = humid / 100.0f;
    int h_leds = (int)std::max(0.0f, std::min(18.0f, h_p * 18.0f));
    count = 0;
    for (int h = 6; h <= 11; h++) {
      for (int s = 1; s <= 3; s++) {
        if (count < h_leds) {
          Color c = get_humid_color(count * (100.0f / 18.0f));
          it[R1_NUM_LEDS + (h * 4) + s] = Color(
            (uint8_t)(c.r * nc.r / 255.0f),
            (uint8_t)(c.g * nc.g / 255.0f),
            (uint8_t)(c.b * nc.b / 255.0f));
        }
        count++;
      }
    }
  }

  void RingClock::render_sensors_temp_glow(light::AddressableLight & it) {
    float temp = _temp_sensor ? _temp_sensor->state : 20.0f;
    Color c  = get_temp_color(temp);
    Color nc = get_cv_color(this->notification_color->current_values);
    Color glow = Color(
      (uint8_t)(c.r * nc.r / 255.0f),
      (uint8_t)(c.g * nc.g / 255.0f),
      (uint8_t)(c.b * nc.b / 255.0f));
    for (int i = 0; i < 12; i++) {
      it[R1_NUM_LEDS + (i * 4) + 1] = glow;
      it[R1_NUM_LEDS + (i * 4) + 2] = glow;
      it[R1_NUM_LEDS + (i * 4) + 3] = glow;
    }
  }

  void RingClock::render_sensors_humid_glow(light::AddressableLight & it) {
    float humid = _humidity_sensor ? _humidity_sensor->state : 50.0f;
    Color c  = get_humid_color(humid);
    Color nc = get_cv_color(this->notification_color->current_values);
    Color glow = Color(
      (uint8_t)(c.r * nc.r / 255.0f),
      (uint8_t)(c.g * nc.g / 255.0f),
      (uint8_t)(c.b * nc.b / 255.0f));
    for (int i = 0; i < 12; i++) {
      it[R1_NUM_LEDS + (i * 4) + 1] = glow;
      it[R1_NUM_LEDS + (i * 4) + 2] = glow;
      it[R1_NUM_LEDS + (i * 4) + 3] = glow;
    }
  }

  void RingClock::render_sensors_dual_glow(light::AddressableLight & it) {
    float temp  = _temp_sensor     ? _temp_sensor->state     : 20.0f;
    float humid = _humidity_sensor ? _humidity_sensor->state : 50.0f;
    Color nc = get_cv_color(this->notification_color->current_values);
    Color tc = get_temp_color(temp);
    Color hc = get_humid_color(humid);
    Color t_glow = Color((uint8_t)(tc.r * nc.r / 255.0f), (uint8_t)(tc.g * nc.g / 255.0f), (uint8_t)(tc.b * nc.b / 255.0f));
    Color h_glow = Color((uint8_t)(hc.r * nc.r / 255.0f), (uint8_t)(hc.g * nc.g / 255.0f), (uint8_t)(hc.b * nc.b / 255.0f));
    for (int h = 0; h <= 5; h++)
      for (int s = 1; s <= 3; s++) it[R1_NUM_LEDS + (h * 4) + s] = t_glow;
    for (int h = 6; h <= 11; h++)
      for (int s = 1; s <= 3; s++) it[R1_NUM_LEDS + (h * 4) + s] = h_glow;
  }

  void RingClock::render_sensors_ticks(light::AddressableLight & it) {
    float temp  = _temp_sensor     ? _temp_sensor->state     : 20.0f;
    float humid = _humidity_sensor ? _humidity_sensor->state : 50.0f;
    Color nc = get_cv_color(this->notification_color->current_values);

    // Temperature tick — right side, 18 positions
    float t_p = std::max(0.0f, std::min(1.0f, (temp + 10.0f) / 60.0f));
    int t_idx = (int)(t_p * 17.99f);
    int count = 0;
    for (int h = 5; h >= 0; h--) {
      for (int s = 3; s >= 1; s--) {
        if (count == t_idx) {
          Color c = get_temp_color(-10.0f + (count * (60.0f / 18.0f)));
          it[R1_NUM_LEDS + (h * 4) + s] = Color(
            (uint8_t)(c.r * nc.r / 255.0f), (uint8_t)(c.g * nc.g / 255.0f), (uint8_t)(c.b * nc.b / 255.0f));
        }
        count++;
      }
    }

    // Humidity tick — left side, 18 positions
    float h_p = std::max(0.0f, std::min(1.0f, humid / 100.0f));
    int h_idx = (int)(h_p * 17.99f);
    count = 0;
    for (int h = 6; h <= 11; h++) {
      for (int s = 1; s <= 3; s++) {
        if (count == h_idx) {
          Color c = get_humid_color(count * (100.0f / 18.0f));
          it[R1_NUM_LEDS + (h * 4) + s] = Color(
            (uint8_t)(c.r * nc.r / 255.0f), (uint8_t)(c.g * nc.g / 255.0f), (uint8_t)(c.b * nc.b / 255.0f));
        }
        count++;
      }
    }
  }

  void RingClock::render_sensors_tick_individual(light::AddressableLight & it, bool is_temp) {
    float val = is_temp
      ? (_temp_sensor     ? _temp_sensor->state     : 20.0f)
      : (_humidity_sensor ? _humidity_sensor->state : 50.0f);
    float p = std::max(0.0f, std::min(1.0f,
      is_temp ? (val + 10.0f) / 60.0f : val / 100.0f));
    Color nc = get_cv_color(this->notification_color->current_values);

    int led_idx = (int)(p * 35.99f);
    int count = 0;
    for (int h = 0; h < 12; h++) {
      for (int s = 1; s <= 3; s++) {
        if (count == led_idx) {
          Color c = is_temp ? get_temp_color(val) : get_humid_color(val);
          it[R1_NUM_LEDS + (h * 4) + s] = Color(
            (uint8_t)(c.r * nc.r / 255.0f), (uint8_t)(c.g * nc.g / 255.0f), (uint8_t)(c.b * nc.b / 255.0f));
        }
        count++;
      }
    }
  }

  void RingClock::render_sensors_bar_individual(light::AddressableLight & it, bool is_temp) {
    float val = is_temp
      ? (_temp_sensor     ? _temp_sensor->state     : 20.0f)
      : (_humidity_sensor ? _humidity_sensor->state : 50.0f);
    float p = is_temp ? (val + 10.0f) / 60.0f : val / 100.0f;
    Color nc = get_cv_color(this->notification_color->current_values);

    int leds = (int)std::max(0.0f, std::min(36.0f, p * 36.0f));
    int count = 0;
    for (int h = 0; h < 12; h++) {
      for (int s = 1; s <= 3; s++) {
        if (count < leds) {
          float current_val = is_temp
            ? (-10.0f + (count * (60.0f / 36.0f)))
            : (count * (100.0f / 36.0f));
          Color c = is_temp ? get_temp_color(current_val) : get_humid_color(current_val);
          it[R1_NUM_LEDS + (h * 4) + s] = Color(
            (uint8_t)(c.r * nc.r / 255.0f), (uint8_t)(c.g * nc.g / 255.0f), (uint8_t)(c.b * nc.b / 255.0f));
        }
        count++;
      }
    }
  }

  void RingClock::render_timer(light::AddressableLight & it) {
    clear_R1(it);
    clear_R2(it);
    draw_markers(it);
    if (!_timer_active) return;

    long remaining_ms = (long)_timer_target_ms - (long)millis();
    if (remaining_ms < 0) remaining_ms = 0;
    int total_seconds = remaining_ms / 1000;
    int hours   = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    if (total_seconds > 0 && total_seconds < 60) {
      Color sc = (second_hand_color && second_hand_color->current_values.get_state())
        ? get_cv_color(second_hand_color->current_values) : _default_second_color;
      for (int i = 0; i < seconds; i++) it[i] = sc;
    } else if (total_seconds > 0) {
      Color hc = (hour_hand_color && hour_hand_color->current_values.get_state())
        ? get_cv_color(hour_hand_color->current_values) : _default_hour_color;
      for (int i = 0; i < 12 && i < hours; i++) it[R1_NUM_LEDS + (i * 4)] = hc;

      Color mc = (minute_hand_color && minute_hand_color->current_values.get_state())
        ? get_cv_color(minute_hand_color->current_values) : _default_minute_color;
      for (int i = 0; i < minutes; i++) it[i] = mc;

      Color sc = (second_hand_color && second_hand_color->current_values.get_state())
        ? get_cv_color(second_hand_color->current_values) : _default_second_color;
      it[seconds] = sc;
    }

    if (total_seconds == 0 && _timer_active) {
      if (_timer_finished_ms == 0) _timer_finished_ms = millis();

      if (!_timer_finishing_dispatched) {
        this->on_timer_finished();
        _timer_finishing_dispatched = true;
      }

      // Pulse the notification ring for configured duration, then clean up
      uint32_t elapsed_finish = millis() - _timer_finished_ms;
      if (elapsed_finish < ALARM_VISUAL_DURATION_MS) {
        float pulse = 0.3f + 0.7f * ((sinf(millis() * 0.003f) + 1.0f) / 2.0f);
        // Use notification_color if on; fall back to white so the pulse
        // is always visible in default clock mode (notification is off).
        Color nc = (this->notification_color != nullptr
                    && this->notification_color->current_values.get_state())
          ? get_cv_color(this->notification_color->current_values)
          : Color(255, 255, 255);
        Color pc = Color((uint8_t)(nc.r * pulse), (uint8_t)(nc.g * pulse), (uint8_t)(nc.b * pulse));
        for (int i = 0; i < 12; i++) {
          int base = R1_NUM_LEDS + (i * 4);
          it[base + 1] = pc; it[base + 2] = pc; it[base + 3] = pc;
        }
      } else {
        // Finished animation complete — reset timer state and return to clock
        _timer_active = false;
        _state = state::time;
      }
    }
  }

  void RingClock::render_stopwatch(light::AddressableLight & it) {
    clear_R1(it);
    clear_R2(it);
    draw_markers(it);

    uint32_t elapsed_ms = _stopwatch_active
      ? (millis() - _stopwatch_start_ms)
      : _stopwatch_paused_ms;
    if (elapsed_ms >= (uint32_t)(12 * 3600 * 1000)) elapsed_ms = 12 * 3600 * 1000 - 1;

    int total_seconds = elapsed_ms / 1000;
    int hours   = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    if (minutes != _stopwatch_last_minute && _stopwatch_active) {
      if (_stopwatch_last_minute != -1) this->on_stopwatch_minute();
      _stopwatch_last_minute = minutes;
    }

    Color hc = (hour_hand_color   && hour_hand_color->current_values.get_state())
      ? get_cv_color(hour_hand_color->current_values)   : _default_hour_color;
    Color mc = (minute_hand_color && minute_hand_color->current_values.get_state())
      ? get_cv_color(minute_hand_color->current_values) : _default_minute_color;
    Color sc = (second_hand_color && second_hand_color->current_values.get_state())
      ? get_cv_color(second_hand_color->current_values) : _default_second_color;

    for (int i = 0; i < 12 && i < hours;   i++) it[R1_NUM_LEDS + (i * 4)] = hc;
    for (int i = 0; i < minutes; i++) it[i] = mc;
    it[seconds] = sc;
  }

  void RingClock::render_alarm(light::AddressableLight & it) {
    float pulse = 0.3f + 0.7f * ((sinf(millis() * 0.003f) + 1.0f) / 2.0f);
    // Use notification_color if on; fall back to white so the alarm
    // pulse is always visible in default clock mode (notification is off).
    Color nc = (this->notification_color != nullptr
                && this->notification_color->current_values.get_state())
      ? get_cv_color(this->notification_color->current_values)
      : Color(255, 255, 255);
    Color pc = Color((uint8_t)(nc.r * pulse), (uint8_t)(nc.g * pulse), (uint8_t)(nc.b * pulse));
    for (int i = 0; i < 12; i++) {
      int base = R1_NUM_LEDS + (i * 4);
      it[base + 1] = pc; it[base + 2] = pc; it[base + 3] = pc;
    }
  }

  // --- Trigger Constructors ---
  ReadyTrigger::ReadyTrigger(RingClock *parent)               { parent->add_on_ready_callback([this]()              { this->trigger(); }); }
  TimerFinishedTrigger::TimerFinishedTrigger(RingClock *p)    { p->add_on_timer_finished_callback([this]()          { this->trigger(); }); }
  StopwatchMinuteTrigger::StopwatchMinuteTrigger(RingClock *p){ p->add_on_stopwatch_minute_callback([this]()        { this->trigger(); }); }
  AlarmTriggeredTrigger::AlarmTriggeredTrigger(RingClock *p)  { p->add_on_alarm_triggered_callback([this]()         { this->trigger(); }); }
  TimerStartedTrigger::TimerStartedTrigger(RingClock *p)      { p->add_on_timer_started_callback([this]()           { this->trigger(); }); }
  TimerStoppedTrigger::TimerStoppedTrigger(RingClock *p)      { p->add_on_timer_stopped_callback([this]()           { this->trigger(); }); }
  StopwatchStartedTrigger::StopwatchStartedTrigger(RingClock *p){ p->add_on_stopwatch_started_callback([this]()     { this->trigger(); }); }
  StopwatchPausedTrigger::StopwatchPausedTrigger(RingClock *p){ p->add_on_stopwatch_paused_callback([this]()        { this->trigger(); }); }
  StopwatchResetTrigger::StopwatchResetTrigger(RingClock *p)  { p->add_on_stopwatch_reset_callback([this]()         { this->trigger(); }); }

} // namespace ring_clock
} // namespace esphome
