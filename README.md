# zmk-driver-animation

![ZMK Version](https://img.shields.io/badge/ZMK-main-blue)
[![Test](https://github.com/cormoran/zmk-driver-animation/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-driver-animation/actions/workflows/zmk-module.yml)

A ZMK module providing RGB LED animation drivers for keyboards.

This module provides configurable LED strip animations including solid colors, battery status, BLE endpoint status, layer status, and composable animation chains.

## Features

- Solid color animations with color cycling and transitions
- Battery status indicator animation
- BLE endpoint/connection status animation
- Layer status indicator animation
- Composable animations (parallel and sequential)
- Animation control behavior for keymap binding
- Animation trigger behavior for keymap binding

## Module User Guide

1. Add dependency to your config/west.yml:

   ```yml
   manifest:
       remotes:
           ...
           - name: cormoran
             url-base: https://github.com/cormoran
       projects:
           ...
           - name: zmk-driver-animation
             remote: cormoran
             revision: main
           ...
   ```

2. Enable animation in your config/\<shield\>.conf:

   ```conf
   CONFIG_ZMK_ANIMATION=y
   ```

3. Configure your device tree overlay with LED strip and animation nodes:

   ```dts
   #include <zmk_driver_animation/animation.dtsi>
   #include <behaviors/animation_control.dtsi>
   #include <dt-bindings/zmk_driver_animation/animation_control.h>

   / {
       chosen {
           zmk,animation = &animation;
           zmk,animation-control = &animation_control0;
       };

       animation: animation {
           compatible = "zmk,animation";
           drivers = <&led_strip>;
           chain-lengths = <1>;
           pixels = <&pixel 0 0>;
       };

       animation_solid0: animation_solid_0 {
           compatible = "zmk,animation-solid";
           pixels = <0>;
           colors = <HSL(120, 100, 50)>;
       };

       animation_control0: animation_control_0 {
           compatible = "zmk,animation-control";
           label = "ANIMATION_CONTROL";
           powered-animations = <&animation_solid0>;
           battery-animations = <&animation_solid0>;
           behavior-animations = <&animation_solid0>;
       };
   };
   ```

4. Add animation control to your keymap:

   ```dts
   bindings = <&animctl ANIMATION_CONTROL_CMD_ENABLE 1>;
   ```

## Development Guide

### Setup

#### Option 1: Isolated layout

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

#### Option 2: Workspace layout

```bash
mkdir west-workspace
cd west-workspace
git clone <this repository>
cd <cloned directory>
west init -l . --mf west/west-test-workspace.yml
west update --narrow
west zephyr-export
```

### Running Tests

```bash
# Run unit test + build test
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
```
