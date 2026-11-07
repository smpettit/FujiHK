# Apple Home controls

FujiHK exposes three services within one accessory:

- **Heater/Cooler:** unit power, heat/cool/automatic operation, and temperature.
- **Fan Speed:** unit power and four manual fan-speed presets.
- **Auto Fan:** an explicit automatic/manual fan-mode switch.

Apple Home decides how to arrange the services. The combined accessory view
and the separate Auto Fan switch were tried on iOS 27.0.1. The Fan service also
exposes HomeKit's native Auto/Manual characteristic for clients that display it.
The extra switch makes the same setting accessible in the tested combined view.

## Fan speed, mode, and power

| Action | Requested behaviour |
| --- | --- |
| Fan Speed Off or 0% | Turn the whole heat pump off |
| Fan Speed On | Turn the unit on using its existing heating/cooling mode and setpoint |
| Select 25% | Turn the unit on with Quiet fan speed |
| Select 50% | Turn the unit on with Low fan speed |
| Select 75% | Turn the unit on with Medium fan speed |
| Select 100% | Turn the unit on with High fan speed |
| Auto Fan On | Select automatic fan speed; leave unit power unchanged |
| Auto Fan Off | Select Manual using the remembered manual speed; leave power unchanged |

Fujitsu has one unit power control. The fan and Heater/Cooler services therefore
share that state: turning either off turns the unit off and updates the other
control. A fan power/speed request does not change the heating/cooling mode or
temperature. A standalone On request keeps the fan mode; selecting a positive
manual speed selects Manual.

**0% means Off, not Auto.** Use Auto Fan to request automatic fan speed. This is
separate from the heat/cool Auto mode, which lets Fujitsu choose heating or cooling.

The percentages represent presets, not measured airflow. While Auto is selected,
the slider retains the remembered manual level; the protocol reports fan mode
Auto but does not provide the instantaneous fan speed chosen by the unit. When
the unit is off, the slider shows 0%. A standalone Off request retains the fan
mode and remembered speed for later use.

The remembered manual speed starts at Low if no manual speed has yet been
selected or observed from the wall controller during this boot. It is not
separately persisted to flash. A valid observed manual setting updates that
memory. Incoming positive percentages between the advertised steps round to
the nearest preset, with a minimum of Quiet; invalid or non-finite values are rejected.

## Synchronisation

Requests are reflected in Home while waiting for controller-bus feedback. Power
and fan changes each allow up to five seconds for a matching observed value;
after that, the best available bus state takes precedence. A new power request
does not interfere with the fan field's wait, and vice versa.

Changes reported by the wall controller update the shared power state, fan
preset, and Auto Fan switch. That implementation still needs a live-bus test.
When the bus is disconnected, controls can be exercised locally for UI review;
those displayed values do not confirm an action on the physical heat pump.

## Validation

On 2026-11-07 the XC3800 firmware was uploaded over USB while all three
heat-pump leads were disconnected. Observed results:

- Both supported board environments compiled successfully.
- HomeSpan reported zero accessory-database warnings and errors on the XC3800.
- The existing Wi-Fi settings and HomeKit pairing remained usable, including
  an authenticated HomeKit client connection.
- The owner confirmed Auto Fan appeared in the combined iOS 27.0.1 view.
- Serial traces showed manual slider changes queuing the fan preset and unit
  On, Off/0% queuing unit Off, and Auto Fan changes queuing no power command.
- The owner accepted the restored Heater/Cooler + Fan Speed + Auto Fan layout.

Regression checks cover shared power, all four manual speeds, return from Auto,
Off/zero preserving fan settings, simultaneous HomeKit writes, invalid inputs,
and observed wall-controller values. Run with a host C++11 compiler from the
repository root:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -Isrc test/fan-control.cpp -o /tmp/fujihk-fan-control-test
/tmp/fujihk-fan-control-test
```

These are model checks, not tests of the physical controller bus or Apple Home's
rendering on other OS versions. Credentials and raw serial logs are excluded.

## Remaining checks

With USB disconnected and the heat-pump connections restored, verify:

1. Secondary-controller binding alongside the original wall controller.
2. Quiet, Low, Medium, High, and Auto on the actual unit.
3. Unit Off/On from both Home controls, with heating/cooling settings retained.
4. Auto Fan switching without changing unit power, including while off.
5. Wall-controller changes appearing correctly in Home.
6. Temperature control and recovery after a normal power cycle.

The existing Heater/Cooler temperature controls are retained. Heat/cool Auto
still uses the upstream midpoint and fixed displayed range; single-setpoint
Auto-mode usability remains separate follow-up work.
