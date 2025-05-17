# ZMK RGB animation driver

The code is based on @ice9js's work in https://github.com/zmkfirmware/zmk/pull/1046.
The implementation is updated to fit to LED indicator usage in dya-dash keyboard.

See Youtube how it can be used with your ZMK keyboard https://youtu.be/y6X2OnoMX-M

## Setup

In your `config/west.yml`:

```yml
manifest:
  remotes:
    ...
    - name: cormoran
      url-base: https://github.com/cormoran
    ...
  projects:
    ...
    - name: zmk-driver-animation
      remote: cormoran
      revision: main
    ...
```

In your `<keyboard>.dtsi`, define animation devices.
Blow is [DYA Dash](https://github.com/cormoran/dya-dash-keyboard)'s example which uses XIAO nRF52840 controller.

- 4 SK6812 (ws2812 compatible)
- `GPIO(0, 10)` (=NFC2) is connected to SK6812's input
- `GPIO(0, 9)` (=NFC1) is connected to FET which controls SK6812's power.

```dst

#include <zmk_driver_animation/animation.dtsi>
#include <zmk_driver_animation/animation_layer_status.dtsi>
#include <dt-bindings/zmk_driver_animation/animation_control.h>
#include <dt-bindings/zmk_driver_animation/animation_trigger.h>
#include <behaviors/animation_trigger.dtsi>


/ {
    chosen {
        // Set root animation device. Suggested device is zmk,animation-control.
        zmk,animation = &root_animation;
        // Set zmk,animation-control device to be accessed from behavior.
        zmk,animation-control = &root_animation;
    };

    // Animation node which controls animation device.
    // LED device and pixel mapping is defined in it.
    animation: animation {
        compatible = "zmk,animation";
        drivers = <&led_strip>;
        pixels = <&pixel 0 0>,
            <&pixel 1 0>,
            <&pixel 2 0>,
            <&pixel 3 0>;
        chain-lengths = <4>;
    };

    // Optionally define zmk,ext-power compatible device to minimize power consumption if your keyboard supports
    led_power:led_power {
        compatible = "zmk,ext-power-transient";
        control-gpios = <&gpio0 9 (GPIO_ACTIVE_LOW)>; // NFC1
    };

    // Root animation played by &animation node. It keeps running during ZMK is active state (and idle state depends on setting).
    // zmk,animation-control plays child animations depending on power status, event and user input.
    root_animation:animation_0 {
        compatible = "zmk,animation-control";
        label = "ANIMATION_CTRL_0";
        powered-animations = <&endpoint_status &rainbow_animation>;
        battery-animations = <&empty_animation>;
        behavior-animations = <&battery_status &endpoint_status &animation_layer_status>;
        init-animation = <&init_animation>;
        activation-animation = <&battery_status>;
        activation-animation-duration-ms = <1000>;
        ext-power = <&led_power>;
    };

    //
    // Animation definitions
    //

    // animation played at start up.
    // zmk,animation-compose can compose multiple animations.
    init_animation:init_animation {
        compatible = "zmk,animation-compose";
        // animations are played sequentially by default. Parallel mode is also supported.
        // Below setting shows battery status animation for 1sec, black for 0.2sec and shows endpoint status for 1sec.
        animations = <&battery_status &black_animation &endpoint_status>;
        durations-ms = <1000 200 1000>;
    };

    black_animation:black_animation {
        compatible = "zmk,animation-solid";
        pixels = <0 1 2 3>;
        colors = <HSL(0, 0, 0)>;
    };

    // zmk,animation-empty is special device. animation-control turns OFF LED module during playing it by setting ext-power OFF.
    empty_animation:empty_animation {
        compatible = "zmk,animation-empty";
        status = "okay";
    };
    rainbow_animation:rainbow_animation {
        compatible = "zmk,animation-solid";
        status = "okay";
        pixels = <0 1 2 3>;
        colors = <HSL(0, 100, 50) HSL(60, 100, 50) HSL(120, 100, 50) HSL(180, 100, 50) HSL(240, 100, 50) HSL(300, 100, 50)>;
    };
    // endpoint status animation
    // The driver also implements endpoint change event handling and animation automatically played when endpoint changed.
    endpoint_status:endpoint_status {
        compatible = "zmk,animation-endpoint";
        status = "okay";
        pixels = <0 1 2 3>;
        color-open = <HSL(60, 100, 50)>;
        color-connected = <HSL(240, 100, 50)>;
        color-disconnected = <HSL(0, 100, 50)>;
        color-usb = <HSL(120, 100, 25)>;
    };
    // Battery status animation
    // The driver also implements battery status change event handling and show warning animation when battery percentage became low.
    battery_status:battery_status {
        compatible = "zmk,animation-battery-status";
        status = "okay";
        pixels = <0 1 2 3>;
        color-high = <HSL(120, 100, 50)>; // green
        color-middle = <HSL(60, 100, 50)>; // yellow
        color-low = <HSL(0, 100, 50)>; // red
    };
};

// Below is LED driver setting. Any stripe-led driver works with animation node.
&pinctrl {
    spi3_default: spi3_default {
        group1 {
            psels = <NRF_PSEL(SPIM_MOSI, 0, 10)>; // NFC2
        };
    };

    spi3_sleep: spi3_sleep {
        group1 {
            psels = <NRF_PSEL(SPIM_MOSI, 0, 10)>;
            low-power-enable;
        };
    };
};

&spi3 {
    compatible = "nordic,nrf-spim";
    status = "okay";

    pinctrl-0 = <&spi3_default>;
    pinctrl-1 = <&spi3_sleep>;
    pinctrl-names = "default", "sleep";

    led_strip: ws2812@0 {
        compatible = "worldsemi,ws2812-spi";

        /* SPI */
        reg = <0>; /* ignored, but necessary for SPI bindings */
        spi-max-frequency = <4000000>;

        /* WS2812 */
        chain-length = <4>; /* number of LEDs */
        spi-one-frame = <0x70>;
        spi-zero-frame = <0x40>;
        color-mapping = <LED_COLOR_ID_GREEN
                         LED_COLOR_ID_RED
                         LED_COLOR_ID_BLUE>;
    };
};

// zmk,animation-layer-status is provided by #include <zmk_driver_animation/animation_layer_status.dtsi> (Why...? I completely forgot reason why only this node is pre-defined...)
// Below code overwrites some fields to work nicely with https://github.com/cormoran/zmk-feature-default-layer
&animation_layer_status {
    pixels = <0 1 2 3>;
    default-color = <HSL(240, 100, 50)>;
    // default
    // windows blue
    // mac red
    // ios white
    // linux orange
    colors = <
        0
        HSL(193, 100, 47)
        HSL(0, 100, 40)
        HSL(0, 100, 100)
        HSL(29, 88, 51)
    >;
};

```

If your keyboard is split keyboard, you might want to define different offset to show different information in left and right.
In case of DYA Dash,

`dya_dash_right.overlay`

```
&animation_layer_status {
    // show layer 4,5,6,7 in right side
    layer-offset = <4>;
};

```

In `dya_dash_left.overlay`, since the PCB design left and right is mirror, pixel definition need to be reverted.
(TODO: Re-define `&animation::pixels` is simpler rather than setting in each animation.)

```
&animation_layer_status {
    pixels = <3 2 1 0>;
};

&endpoint_status {
    pixels = <3 2 1 0>;
};

&battery_status {
    pixels = <3 2 1 0>;
};
```

In your <keyboard>.keymap, you can use `animtrig` and `animctl` behavior.

```keymap
#include <behaviors/animation_control.dtsi>
#include <behaviors/animation_trigger.dtsi>
#include <dt-bindings/zmk_driver_animation/animation_control.h>
#include <dt-bindings/zmk_driver_animation/animation_trigger.h>

bindings = <
...
&animtrig ANM_TRG 0 # trigger behavior-animations[0] defined in your zmk,animation-control node
&animctl ANM_BRI # brightness++
&animctl ANM_BRD # brightness--
&animctl ANM_EN # Enable animation
&animctl ANM_DS # Disable animation
&animctl ANM_INC # Change animation to powered-animations[i+1] or battery-animations[i+1] depending on current power status
...
>
```

## TODO

- [ ] Write test. I believe the implementation has many bugs and timing issues.
- [ ] Consider giving feedback/opinion to original work https://github.com/zmkfirmware/zmk/pull/1046..
