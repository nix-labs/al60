# NIX labs AL60 Clock Firmware

This repository contains the ESPHome firmware for the NIX labs AL60 Clock, an advanced dual-ring LED clock powered by an ESP32-C3 and ESPHome.

## Repository Structure

- **[al60.yaml](file:///Users/nathan/Documents/GitHub/nixr_dev/al60.yaml)**: Main entry point for standard users (pulls remote packages).
- **[al60_factory.yaml](file:///Users/nathan/Documents/GitHub/nixr_dev/al60_factory.yaml)**: Entry point for development/factory use (uses local packages).
- **[packages/](file:///Users/nathan/Documents/GitHub/nixr_dev/packages/)**: Modular configuration blocks:
  - `al60_core.yaml`: System setup, ESP32-C3 performance tuning.
  - `al60_light.yaml`: LED effects and dummy controls for UI.
  - `al60_inputs.yaml`: Physical buttons and user interaction logic.
  - `al60_sensors.yaml`: Hardware sensors (AHT20, LDR, occupancy).
  - `al60_time.yaml`: RTC and SNTP time synchronization.
  - `al60_radar.yaml`: Optional UART integration for LD2410.
- **[components/ring_clock/](file:///Users/nathan/Documents/GitHub/nixr_dev/components/ring_clock/)**: Custom C++ component driving the clock rendering.

## Using the `ring_clock` Component in Other Projects

The `ring_clock` component can be reused in other ESPHome projects featuring a dual-ring LED layout (60 inner, 48 outer).

### YAML Configuration

Add the component to your configuration. Note that the component depends on existing time and light entities:

```yaml
external_components:
  - source: github://n33174/nixr_dev@main
    components: [ring_clock]

ring_clock:
  id: RingClock
  # Replace all IDs below with your own
  time_id: sntp_time
  light_id: ring_light
  hour_sweep_switch: hour_sweep
  hour_hand_color: hour_hand_color
  minute_hand_color: minute_hand_color
  second_hand_color: second_hand_color
  marker_color: marker_color
  notification_color: notification_color
  sound_enabled_switch: timer_sounds
  temperature_sensor: temp_sensor
  humidity_sensor: humidity_sensor
```

## YAML Customisation

If you have purchased a NIX labs AL60 Clock, you can customize its behavior by importing the config on your own esphome instance and editing as needed.

### Core Settings

Modify the substitutions block at the top of the al60.yaml file:

- name: Network hostname.
- friendly_name: Display name in Home Assistant.
- time_zone: Fallback POSIX timezone string.

### Packages

The firmware is composed of multiple YAML packages in `packages/`:

- **`al60_core.yaml`**: Base board definitions, power limits, and logging settings.
- **`al60_light.yaml`**: Color configurations, dummy placeholders, and visual effect loops.
- **`al60_sensors.yaml`**: Environmental calibrations. Adjust `temp_offset_default` to tune the temperature readout.
- **`al60_inputs.yaml`**: Touch controls and hardware interaction mappings.
- **`al60_time.yaml`**: RTC and SNTP time synchronization.
- **`al60_radar.yaml`**: Optional UART integration for LD2410.

## Documentation

For further reading, hardware schematics, and troubleshooting, visit our resources:

- [General User Documentation](https://docs.nixlabs.com.au/al60)
- [Advanced Technical Customisation](https://docs.nixlabs.com.au/al60/advanced/)

## License

This project is licensed under the **GNU General Public License v3.0 (GPLv3)**.

You are free to use, modify, and distribute the code and precompiled firmware. Any distributed derivative works (including compiled binaries) must also be released under the GPLv3.
