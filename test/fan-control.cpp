#include "fan-control.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <utility>

int main() {
    FanControl::Command command;
    for (bool power : {false, true}) {
        for (uint8_t setting = 1; setting <= 4; ++setting) {
            FanControl fan;
            fan.observePower(power);
            assert(fan.observeFan(setting));
            FanControl::Write mode;
            mode.targetUpdated = true;
            mode.target = 1;
            assert(fan.apply(mode, command));
            assert(fan.automatic() && command.fanUpdated && !command.powerUpdated);
            assert(fan.powered() == power);
            mode.target = 0;
            assert(fan.apply(mode, command));
            assert(fan.setting() == setting && !command.powerUpdated);
            assert(fan.powered() == power);
        }
    }

    for (uint8_t setting = 0; setting <= 4; ++setting) {
        FanControl fan;
        fan.observeFan(setting);
        fan.observePower(true);
        int rememberedSpeed = fan.manualSpeed();
        FanControl::Write off;
        off.activeUpdated = true;
        off.active = false;
        off.speedUpdated = true;
        off.speed = 100;  // A bundled retained slider value cannot override Off.
        assert(fan.apply(off, command));
        assert(command.powerUpdated && !command.power && !command.fanUpdated);
        assert(!fan.powered() && fan.displayedSpeed() == 0);
        assert(fan.setting() == setting && fan.manualSpeed() == rememberedSpeed);

        FanControl::Write on;
        on.activeUpdated = true;
        on.active = true;
        assert(fan.apply(on, command));
        assert(command.powerUpdated && command.power && !command.fanUpdated);
        assert(fan.setting() == setting && fan.displayedSpeed() == rememberedSpeed);

        FanControl::Write zero;
        zero.speedUpdated = true;
        zero.speed = 0;
        assert(fan.apply(zero, command));
        assert(command.powerUpdated && !command.power && !command.fanUpdated);
        assert(fan.setting() == setting);  // Zero never selects Auto.
    }

    FanControl fan;
    // Explicit Auto with a bundled remembered speed does not start an off unit.
    FanControl::Write automatic;
    automatic.targetUpdated = true;
    automatic.target = 1;
    automatic.speedUpdated = true;
    automatic.speed = 75;
    assert(fan.apply(automatic, command));
    assert(fan.automatic() && !fan.powered() && !command.powerUpdated);
    automatic.target = 0;
    assert(fan.apply(automatic, command));
    assert(fan.setting() == 3 && !fan.powered() && !command.powerUpdated);

    for (const auto &sample : {std::pair<float, int>{1, 1}, {12, 1}, {25, 1},
                              {37.4f, 1}, {37.5f, 2}, {62.5f, 3}, {100, 4}}) {
        fan.observePower(false);
        FanControl::Write speed;
        speed.speedUpdated = true;
        speed.speed = sample.first;
        assert(fan.apply(speed, command));
        assert(fan.setting() == sample.second && fan.powered());
        assert(command.fanUpdated && command.powerUpdated && command.power);
    }

    for (float invalid : {-1.0f, 101.0f, std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity()}) {
        FanControl previous = fan;
        FanControl::Write speed;
        speed.speedUpdated = true;
        speed.speed = invalid;
        assert(!fan.apply(speed, command));
        assert(!command.powerUpdated && !command.fanUpdated);
        assert(fan.setting() == previous.setting() && fan.powered() == previous.powered());
        assert(fan.manualSpeed() == previous.manualSpeed());
    }

    assert(fan.observeFan(1));
    assert(fan.observeFan(0));
    fan.observePower(false);  // Wall-controller power and fan settings are separate.
    assert(fan.automatic() && !fan.powered() && fan.manualSpeed() == 25);
    assert(!fan.observeFan(255));
    assert(fan.automatic());
    std::cout << "Shared power, zero/Off, Auto/Manual, bundled-write and remote-update checks passed.\n";
}
