#pragma once

#include <cmath>
#include <cstdint>

// The fan and heater/cooler share unit power; Auto is a separate fan setting.
class FanControl {
public:
    struct Write {
        bool activeUpdated = false;
        bool active = false;
        bool targetUpdated = false;
        int target = 1;  // HomeKit: Manual=0, Auto=1
        bool speedUpdated = false;
        float speed = 0;
    };

    struct Command {
        bool powerUpdated = false;
        bool power = false;
        bool fanUpdated = false;
        uint8_t fan = 0;
    };

    bool apply(const Write &write, Command &command) {
        command = Command{};
        if ((write.targetUpdated && write.target != 0 && write.target != 1) ||
            (write.speedUpdated && (!std::isfinite(write.speed) || write.speed < 0 || write.speed > 100))) {
            return false;
        }

        // Off wins when Home bundles Active with a retained non-zero speed.
        bool stop = (write.activeUpdated && !write.active) || (write.speedUpdated && write.speed == 0);
        bool positiveSpeed = write.speedUpdated && write.speed > 0;
        if (positiveSpeed && !stop) {
            int level = static_cast<int>(std::lround(write.speed / 25));
            lastManual = static_cast<uint8_t>(level < 1 ? 1 : level);
        }

        if (write.targetUpdated) {
            selected = write.target == 1 ? 0 : lastManual;
            command.fanUpdated = true;
        } else if (positiveSpeed && !stop) {
            selected = lastManual;
            command.fanUpdated = true;
        }
        command.fan = selected;

        // Changing only Auto/Manual must not start the unit, including when
        // Home bundles the mode change with its remembered slider value.
        if (stop) {
            power = false;
            command.powerUpdated = true;
        } else if ((write.activeUpdated && write.active) || (positiveSpeed && !write.targetUpdated)) {
            power = true;
            command.powerUpdated = true;
        }
        command.power = power;
        return true;
    }

    bool observeFan(uint8_t setting) {
        if (setting > 4) {
            return false;
        }
        selected = setting;
        if (setting != 0) {
            lastManual = setting;
        }
        return true;
    }

    void observePower(bool on) { power = on; }
    uint8_t setting() const { return selected; }
    bool automatic() const { return selected == 0; }
    bool powered() const { return power; }
    int manualSpeed() const { return lastManual * 25; }
    int displayedSpeed() const { return power ? manualSpeed() : 0; }

private:
    uint8_t selected = 0;
    uint8_t lastManual = 2;  // Low until a manual speed has been selected/observed.
    bool power = false;
};
