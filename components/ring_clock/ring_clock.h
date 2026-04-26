#pragma once

#include "esphome/components/light/addressable_light.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/automation.h"
#include "esphome/core/color.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <vector>

// Hardware Definition
// The clock consists of two rings of WS2812 LEDs
// R1 (Inner Ring): 60 LEDs (Minutes/Seconds) - Indices 0-59
// R2 (Outer Ring): 48 LEDs (Hours/Markers) - Indices 60-107
#define TOTAL_LEDS 108
#define R1_NUM_LEDS 60
#define R2_NUM_LEDS 48

// Indices of LEDs physically adjacent to the light sensor on the PCB.
// Used to estimate LED interference when reading ambient brightness.
// Update these if the PCB layout changes.
#define SENSOR_ADJACENT_LED_R1 15 // Inner ring LED nearest the sensor
#define SENSOR_ADJACENT_LED_R2                                                 \
  12 // Outer ring LED nearest the sensor (R2-relative index)

// Maximum timer duration: 12 h 59 m 59 s expressed in seconds
#define TIMER_MAX_SECONDS 46799

// Duration in milliseconds for the visual alarm/timer pulse animation.
// Adjust this value to change how long the clock flashes.
static const uint32_t ALARM_VISUAL_DURATION_MS = 10000;

namespace esphome {
namespace ring_clock {

// Tag for ESPHome logging
extern const char *TAG;

// Default Colors applied if no custom overrides are set
static const Color DEFAULT_COLOR_HOUR(255, 145, 0);         // Orange
static const Color DEFAULT_COLOR_MINUTE(0, 255, 255);       // Cyan
static const Color DEFAULT_COLOR_SECOND(200, 200, 200);     // White
static const Color DEFAULT_COLOR_NOTIFICATION(255, 145, 0); // Orange
static const Color DEFAULT_COLOR_MARKERS(50, 50, 50);       // Dim White

// Finite State Machine for the Clock's visual mode
enum state {
  time,      // Standard clock display
  time_fade, // Clock with smooth fading seconds
  time_tail, // Clock with 15-LED trailing seconds (colours from individual
             // lights)
  // Note: hour-sweep is driven by the hour_sweep_switch via should_sweep()
  // inside render_time / render_tail — it is not a separate FSM state.
  timer,     // Countdown timer visualization
  stopwatch, // Stopwatch visualization
  alarm,     // Alarm active state (rendered as an overlay when _alarm_active is
             // set)

  // Sensor visualizations
  sensors_bars,       // Dual bars: Temp (Right), Humid (Left)
  sensors_temp_glow,  // Full ring glow based on temperature color
  sensors_humid_glow, // Full ring glow based on humidity color
  sensors_ticks,      // Single pixel indicators for values
  sensors_dual_glow,  // Split glow: Temp (Right), Humid (Left)
  sensors_temp_bar,   // Single full-circle bar for temperature
  sensors_humid_bar,  // Single full-circle bar for humidity
  sensors_temp_tick,  // Single tick for temperature
  sensors_humid_tick, // Single tick for humidity
};

enum MarkerHighlightMode {
  NONE = 0,
  TWELVE_ONLY = 1,
  TWELVE_THREE_SIX_NINE = 2,
};

struct ColorPoint {
  float value;
  Color color;
};

class RingClock : public Component {
public:
  // --- Component Lifecycle ---
  void setup() override;
  void loop() override;
  void on_ready();
  void add_on_ready_callback(std::function<void()> callback);

  // --- Core Rendering ---
  // Main entry point called by the Light Lambda in YAML
  void addressable_lights_lambdacall(light::AddressableLight &it);

  // --- State Management ---
  state get_state();
  void set_state(state state);

  // --- Data Setters (Usually called from YAML) ---
  void set_time(time::RealTimeClock *time);
  void set_hour_sweep_switch(switch_::Switch *hour_sweep) {
    this->_hour_sweep_switch = hour_sweep;
  }
  void set_clock_addressable_lights(light::LightState *it);
  void set_sound_enabled_state(switch_::Switch *sound_enabled) {
    this->_sound_enabled_switch = sound_enabled;
  }

  // Sensor Linking
  void set_temperature_sensor(sensor::Sensor *temp) {
    this->_temp_sensor = temp;
  }
  void set_humidity_sensor(sensor::Sensor *humid) {
    this->_humidity_sensor = humid;
  }

  // Color Customization via LightStates (Dummy Lights in YAML)
  void set_hour_hand_color_state(light::LightState *state);
  void set_minute_hand_color_state(light::LightState *state);
  void set_second_hand_color_state(light::LightState *state);
  void set_marker_color_state(light::LightState *state);
  void set_notification_color_state(light::LightState *state);
  void set_marker_highlight_mode(MarkerHighlightMode mode) {
    this->_marker_highlight_mode = mode;
  }

  // API to define LEDs that should be turned off (hardware masking)
  void set_blank_leds(std::vector<int> leds);

  // Returns a calculated interference factor for the light sensor
  // based on the brightness of LEDs physically near the sensor
  float get_interference_factor();

  // Set the target brightness for smooth transitions.
  // target : 0.0–1.0  — step smoothly toward this brightness.
  //         -1.0       — manual mode (HA slider controls brightness).
  void set_target_brightness(float target);

  // --- Timer Logic ---
  void start_timer(int hours, int minutes, int seconds);
  void stop_timer();
  void on_timer_started();
  void on_timer_stopped();
  void on_timer_finished();

  // --- Time Management API ---
  // Apply SNTP UTC epoch - gated by _sntp_enabled.
  // Call this from sntp on_time_sync instead of settimeofday directly.
  void apply_sntp_sync(time_t utc_epoch);

  // Set time from user-entered local datetime fields.
  // Internally derives the current timezone offset from _time and applies
  // the correct UTC epoch. Also disables sntp sync (manual mode).
  void set_manual_time(int year, int month, int day, int hour, int minute,
                       int second);

  // Increment hour (+3600 s) or minute (+60 s) and zero the seconds.
  void increment_hour();
  void increment_minute();

  // Control whether incoming SNTP syncs are applied to the system clock.
  // Safe to call at any time, including before network is available.
  void set_sntp_enabled(bool enabled);
  bool get_sntp_enabled() const;

  // Call from wifi: on_connect once the TCP/IP stack is running.
  // Applies any pending sntp_stop() that couldn't run at boot time.
  void set_network_ready();

  void add_on_timer_started_callback(std::function<void()> callback);
  void add_on_timer_stopped_callback(std::function<void()> callback);
  void add_on_timer_finished_callback(std::function<void()> callback);

  // --- Stopwatch Logic ---
  void start_stopwatch();
  void pause_stopwatch();
  void stop_stopwatch();
  void reset_stopwatch();
  void on_stopwatch_started();
  void on_stopwatch_paused();
  void on_stopwatch_reset();
  void on_stopwatch_minute();

  void add_on_stopwatch_started_callback(std::function<void()> callback);
  void add_on_stopwatch_paused_callback(std::function<void()> callback);
  void add_on_stopwatch_reset_callback(std::function<void()> callback);
  void add_on_stopwatch_minute_callback(std::function<void()> callback);

  // --- Alarm Logic ---
  void start_alarm();
  void on_alarm_triggered();
  void add_on_alarm_triggered_callback(std::function<void()> callback);

  // --- Default Color Setters (Optional overrides) ---
  void set_default_hour_color(Color color) { _default_hour_color = color; }
  void set_default_minute_color(Color color) { _default_minute_color = color; }
  void set_default_second_color(Color color) { _default_second_color = color; }
  void set_default_notification_color(Color color) {
    _default_notification_color = color;
  }
  void set_default_marker_color(Color color) { _default_marker_color = color; }

  void add_temperature_color_point(float value, Color color) {
    _temp_color_points.push_back({value, color});
  }
  void add_humidity_color_point(float value, Color color) {
    _humid_color_points.push_back({value, color});
  }

protected:
  state _state{state::time};
  bool _has_time{false};
  float _interference_factor{0.0f};
  std::vector<int> _blanked_leds;

  time::RealTimeClock *_time;
  switch_::Switch *_hour_sweep_switch{nullptr};
  switch_::Switch *_sound_enabled_switch{nullptr};
  sensor::Sensor *_temp_sensor{nullptr};
  sensor::Sensor *_humidity_sensor{nullptr};

  light::LightState *_clock_lights{nullptr};
  light::LightState *hour_hand_color{nullptr};
  light::LightState *minute_hand_color{nullptr};
  light::LightState *second_hand_color{nullptr};
  light::LightState *marker_color{nullptr};
  light::LightState *notification_color{nullptr};

  Color _default_hour_color = DEFAULT_COLOR_HOUR;
  Color _default_minute_color = DEFAULT_COLOR_MINUTE;
  Color _default_second_color = DEFAULT_COLOR_SECOND;
  Color _default_notification_color = DEFAULT_COLOR_NOTIFICATION;
  Color _default_marker_color = DEFAULT_COLOR_MARKERS;
  MarkerHighlightMode _marker_highlight_mode{NONE};

  // Color gradient points — sorted once in setup() so per-frame lookups are
  // cheap
  std::vector<ColorPoint> _temp_color_points;
  std::vector<ColorPoint> _humid_color_points;

  int last_second{-1};
  uint32_t last_second_timestamp{0};

  // --- Helpers ---
  void clear_R1(light::AddressableLight &it);
  void clear_R2(light::AddressableLight &it);

  // Resolves the display color for one clock hand from its LightState.
  // Handles Rainbow / Temperature Color / Humidity Color effects; falls back
  // to default_color when the light is off or has no recognised effect.
  // Set is_minute_complement=true to shift Rainbow hue by 180° (minute hand).
  Color resolve_hand_color(light::LightState *ls, Color default_color,
                           const esphome::ESPTime &now,
                           bool is_minute_complement = false);

  // Draws the hour hand on R2 as a single marker LED or smoothly swept
  // between adjacent LEDs when the hour-sweep switch is on.
  void draw_hour_hand(light::AddressableLight &it, Color color,
                      const esphome::ESPTime &now);

  void draw_markers(light::AddressableLight &it);
  void render_time(light::AddressableLight &it, bool fade,
                   const esphome::ESPTime &now);
  void render_tail(light::AddressableLight &it, const esphome::ESPTime &now);
  void render_timer(light::AddressableLight &it);
  void render_stopwatch(light::AddressableLight &it);
  void render_alarm(light::AddressableLight &it);

  void render_sensors_bars(light::AddressableLight &it);
  void render_sensors_ticks(light::AddressableLight &it);
  void render_sensors_temp_glow(light::AddressableLight &it);
  void render_sensors_humid_glow(light::AddressableLight &it);
  void render_sensors_dual_glow(light::AddressableLight &it);
  void render_sensors_bar_individual(light::AddressableLight &it, bool is_temp);
  void render_sensors_tick_individual(light::AddressableLight &it,
                                      bool is_temp);

  bool should_sweep();

private:
  // Gregorian calendar fields -> Unix UTC epoch.
  // Pure arithmetic: no libc TZ side-effects.
  static time_t fields_to_epoch(int year, int month, int day, int hour,
                                int minute, int second);

  Color get_temp_color(float t);
  Color get_humid_color(float h);

  void draw_tail(light::AddressableLight &it, float precise_pos, Color color);
  void draw_fade(light::AddressableLight &it, float precise_pos, Color color);

  // --- Timer State ---
  bool _timer_active{false};
  uint32_t _timer_target_ms{0};
  uint32_t _timer_duration_ms{0};
  uint32_t _timer_finished_ms{0};
  bool _timer_finishing_dispatched{false};

  // --- SNTP Sync Gate ---
  bool _sntp_enabled{true};
  bool _network_ready{false}; // true once TCP/IP stack is up (WiFi connected)

  // --- Stopwatch State ---
  bool _stopwatch_active{false};
  uint32_t _stopwatch_start_ms{0};
  uint32_t _stopwatch_paused_ms{0};
  int _stopwatch_last_minute{-1};

  // --- Alarm State ---
  bool _alarm_active{false};
  uint32_t _alarm_triggered_ms{0};
  bool _alarm_dispatched{false};

  // --- Render Cache ---
  int _cache_h{-1};
  int _cache_m{-1};
  int _cache_s{-1};
  state _cache_mode{state::time};

  // --- Smooth Brightness State ---
  static constexpr float BRIGHTNESS_SPEED{
      0.5f}; // units/second; 50%→75% ≈ 1 second
  static constexpr uint32_t BRIGHTNESS_STEP_MS{
      25}; // min ms between loop() steps (≤ 40 fps)
  float _brightness_target{-1.0f};
  float _brightness_current{-1.0f};
  uint32_t _brightness_step_ms{0};

  // --- Callback Managers ---
  CallbackManager<void()> _on_ready_callback_;
  CallbackManager<void()> _on_timer_finished_callback_;
  CallbackManager<void()> _on_stopwatch_minute_callback_;
  CallbackManager<void()> _on_timer_started_callback_;
  CallbackManager<void()> _on_timer_stopped_callback_;
  CallbackManager<void()> _on_stopwatch_started_callback_;
  CallbackManager<void()> _on_stopwatch_paused_callback_;
  CallbackManager<void()> _on_stopwatch_reset_callback_;
  CallbackManager<void()> _on_alarm_triggered_callback_;
};

// --- Triggers ---
class ReadyTrigger : public Trigger<> {
public:
  explicit ReadyTrigger(RingClock *parent);
};
class TimerFinishedTrigger : public Trigger<> {
public:
  explicit TimerFinishedTrigger(RingClock *parent);
};
class StopwatchMinuteTrigger : public Trigger<> {
public:
  explicit StopwatchMinuteTrigger(RingClock *parent);
};
class AlarmTriggeredTrigger : public Trigger<> {
public:
  explicit AlarmTriggeredTrigger(RingClock *parent);
};
class TimerStartedTrigger : public Trigger<> {
public:
  explicit TimerStartedTrigger(RingClock *parent);
};
class TimerStoppedTrigger : public Trigger<> {
public:
  explicit TimerStoppedTrigger(RingClock *parent);
};
class StopwatchStartedTrigger : public Trigger<> {
public:
  explicit StopwatchStartedTrigger(RingClock *parent);
};
class StopwatchPausedTrigger : public Trigger<> {
public:
  explicit StopwatchPausedTrigger(RingClock *parent);
};
class StopwatchResetTrigger : public Trigger<> {
public:
  explicit StopwatchResetTrigger(RingClock *parent);
};

} // namespace ring_clock
} // namespace esphome
