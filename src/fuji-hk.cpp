#include <Arduino.h>
#include <HomeSpan.h>
#include <FujiHeatPump.h>
#include "fan-control.h"


#define NUM_LEDS 1
#ifndef LED_PIN
#define LED_PIN 27
#endif
#define LED_BRIGHTNESS 1
#ifndef BTN_PIN
#define BTN_PIN 39
#endif

#define RX_PIN 19
#define TX_PIN 22

#define COLOR_RED       255,0,0
#define COLOR_GREEN     0,255,0
#define COLOR_GREEN_DIM 0,1,0
#define COLOR_BLUE      0,0,255
#define COLOR_MAGENTA   162,0,162
#define COLOR_YELLOW    162,162,0
#define COLOR_CYAN      0,168,168
#define COLOR_CYAN_HSV  180,100,66
#define COLOR_ORANGE    255,115,0

TaskHandle_t FujiTask; // task for handling fuji heatpump frames

typedef struct FujiStatus {
    byte isBound = 0;
    byte onOff = 0;
    byte acMode = 5;

    byte temperature = 16;
    byte fanMode = 0;
    
    byte economyMode = 0;
    byte swingMode = 0;
    byte swingStep = 0;
    byte controllerTemp = 16;

    byte acError = 0;
} FujiStatus;

FujiStatus sharedState;
FujiStatus pendingState;
byte       pendingFields = 0;
bool       hpIsBound = false;
bool       wifiConnected = false;
bool       wifiConnecting = false;
bool       needsPairing = false;
bool       inCmdMode = false;

SemaphoreHandle_t updateMutex;
SemaphoreHandle_t pendingMutex;

enum class HeatingCoolerState {
  HeatCool=0,
  Heat=1,
  Cool=2,
};

enum class CurrentHeatingCoolerState {
  Inactive=0,
  Idle=1,
  Heating=2,
  Cooling=3,
};

char sn[32];
char acName[64];

FanControl fanControl;
bool awaitingFanEcho = false;
bool awaitingPowerEcho = false;
unsigned long fanRequestedAt = 0;
unsigned long powerRequestedAt = 0;

bool acceptBusValue(bool &awaiting, unsigned long requestedAt, bool matches) {
    if (awaiting && !matches && millis() - requestedAt < 5000) {
        return false;
    }
    awaiting = false;
    return true;
}

void syncControlsFromBus() {
    if (xSemaphoreTake(updateMutex, 0) != pdTRUE) {
        return;
    }
    if (sharedState.isBound) {
        // Each field waits for its own acknowledgement so an old bus frame
        // cannot immediately undo a new power or fan request in the Home UI.
        if (acceptBusValue(awaitingFanEcho, fanRequestedAt, sharedState.fanMode == fanControl.setting())) {
            fanControl.observeFan(sharedState.fanMode);
        }
        bool on = sharedState.onOff == 1;
        if (acceptBusValue(awaitingPowerEcho, powerRequestedAt, on == fanControl.powered())) {
            fanControl.observePower(on);
        }
    }
    xSemaphoreGive(updateMutex);
}


boolean queueFanWrite(const FanControl::Write &write, const char *source) {
    FanControl next = fanControl;
    FanControl::Command command;
    if (!next.apply(write, command) || xSemaphoreTake(pendingMutex, (TickType_t)200) != pdTRUE) {
        return false;
    }

    if (command.fanUpdated) {
        pendingState.fanMode = command.fan;
        pendingFields |= kFanModeUpdateMask;
        awaitingFanEcho = true;
        fanRequestedAt = millis();
    }
    if (command.powerUpdated) {
        pendingState.onOff = command.power ? 1 : 0;
        pendingFields |= kOnOffUpdateMask;
        awaitingPowerEcho = true;
        powerRequestedAt = millis();
    }
    xSemaphoreGive(pendingMutex);
    fanControl = next;
    Serial.printf("%s: activeWrite=%d targetWrite=%d speedWrite=%d -> setting=%u power=%d powerCommand=%d\n",
              source, write.activeUpdated ? static_cast<int>(write.active) : -1,
              write.targetUpdated ? write.target : -1,
              write.speedUpdated ? static_cast<int>(write.speed) : -1,
              static_cast<unsigned int>(fanControl.setting()),
              static_cast<int>(fanControl.powered()), static_cast<int>(command.powerUpdated));
    return true;
}

struct FujitsuFan : Service::Fan {
    SpanCharacteristic *active;
    SpanCharacteristic *target;
    SpanCharacteristic *speed;

    FujitsuFan() : Service::Fan() {
        new Characteristic::ConfiguredName("Fan Speed");
        syncControlsFromBus();
        active = new Characteristic::Active(fanControl.powered());
        target = new Characteristic::TargetFanState(fanControl.automatic() ? 1 : 0);
        speed = new Characteristic::RotationSpeed(fanControl.displayedSpeed());
        speed->setRange(0, 100, 25);
    }

    boolean update() override {
        if (!active->updated() && !target->updated() && !speed->updated()) {
            return true;
        }
        FanControl::Write write;
        write.activeUpdated = active->updated();
        write.active = active->getNewVal() != 0;
        write.targetUpdated = target->updated();
        write.target = target->getNewVal();
        write.speedUpdated = speed->updated();
        write.speed = speed->getNewVal<float>();
        return queueFanWrite(write, "Fan Speed");
    }

    void loop() override {
        // Publish after update() returns and HomeSpan commits the incoming
        // values. Both services use this same state, including off-bus testing.
        if (active->getVal() != fanControl.powered()) {
            active->setVal(fanControl.powered());
        }
        int mode = fanControl.automatic() ? 1 : 0;
        if (target->getVal() != mode) {
            target->setVal(mode);
        }
        if (speed->getVal<float>() != fanControl.displayedSpeed()) {
            speed->setVal(fanControl.displayedSpeed());
        }
    }
};

struct FujitsuFanAuto : Service::Switch {
    SpanCharacteristic *automatic;

    FujitsuFanAuto() : Service::Switch() {
        new Characteristic::ConfiguredName("Auto Fan");
        automatic = new Characteristic::On(fanControl.automatic());
    }

    boolean update() override {
        if (!automatic->updated()) {
            return true;
        }
        FanControl::Write write;
        write.targetUpdated = true;
        write.target = automatic->getNewVal() ? 1 : 0;
        return queueFanWrite(write, "Auto Fan");
    }

    void loop() override {
        if (automatic->getVal() != fanControl.automatic()) {
            automatic->setVal(fanControl.automatic());
        }
    }
};


struct FujitsuHK : Service::HeaterCooler { 

    SpanCharacteristic *active;
    SpanCharacteristic *currentTemp;
    SpanCharacteristic *currentHeaterCoolerState;
    SpanCharacteristic *targetHeaterCoolerState;
    SpanCharacteristic *coolingThresholdTemperature;
    SpanCharacteristic *heatingThresholdTemperature;
    SpanCharacteristic *temperatureDisplayUnits;


    FujitsuHK() : Service::HeaterCooler(){       // constructor() method

      Serial.print("Init FujitsuHK\n");           // initialization message

      FujiStatus currentState;
      
      if(xSemaphoreTake(updateMutex, ( TickType_t ) 200 ) == pdTRUE) {
        memcpy(&currentState, &sharedState, sizeof(FujiStatus));
        xSemaphoreGive(updateMutex);
      } else {
        Serial.print("Failed to get MUTEX lock, using defaults\n"); 
      }

      syncControlsFromBus();
      active = new Characteristic::Active(fanControl.powered());
      
      currentTemp = new Characteristic::CurrentTemperature(currentState.isBound ? currentState.controllerTemp : 22);
      currentTemp->setRange(0, 100, 1);

      currentHeaterCoolerState = new Characteristic::CurrentHeaterCoolerState(static_cast<int>(CurrentHeatingCoolerState::Idle));
      
      int hkMode = convertACModeToHKMode(currentState.isBound ? currentState.acMode : 0);
      targetHeaterCoolerState = new Characteristic::TargetHeaterCoolerState(hkMode);

      coolingThresholdTemperature = new Characteristic::CoolingThresholdTemperature(currentState.isBound ? currentState.temperature : 22);
      coolingThresholdTemperature->setRange(16, 30, 1);
      
      heatingThresholdTemperature = new Characteristic::HeatingThresholdTemperature(currentState.isBound ? currentState.temperature : 22);
      heatingThresholdTemperature->setRange(16, 30, 1);
      
      temperatureDisplayUnits = new Characteristic::TemperatureDisplayUnits(0);

    } // end constructor

    int convertACModeToHKMode(byte acMode) {
      
      switch(acMode) {
        case static_cast<byte>(FujiMode::COOL):
          return static_cast<int>(HeatingCoolerState::Cool);
          break;
        case static_cast<byte>(FujiMode::HEAT):
          return static_cast<int>(HeatingCoolerState::Heat);
          break;
        case static_cast<byte>(FujiMode::AUTO):
          return static_cast<int>(HeatingCoolerState::HeatCool);
      }

      Serial.printf("UNSUPPORTED MODE IN convertACModeToHKMode %d! RETURNING 0\n", acMode);
      return 0;
    }

    byte convertHKModeToACMode(int hkMode) {
      
      switch(hkMode) {
        case static_cast<int>(HeatingCoolerState::Cool):
          return static_cast<byte>(FujiMode::COOL);          
        case static_cast<int>(HeatingCoolerState::Heat):
          return static_cast<byte>(FujiMode::HEAT);
        case static_cast<int>(HeatingCoolerState::HeatCool):
          return static_cast<byte>(FujiMode::AUTO);
      }

      Serial.printf("UNSUPPORTED MODE in convertHKModeToACMode %d! RETURNING 5\n", hkMode);
      return 5;
    }


    void loop(){
      // The fan tile and heater/cooler tile represent the same unit power.
      if (active->getVal() != fanControl.powered()) {
        active->setVal(fanControl.powered());
      }
      FujiStatus currentState;

      if(pendingFields) { // if there are pending updates skip the loop to avoid UI bounces
        return;
      }

      if(xSemaphoreTake( updateMutex, ( TickType_t ) 200 ) == pdTRUE) {
        memcpy(&currentState, &sharedState, sizeof(FujiStatus));
        xSemaphoreGive( updateMutex);
      } else {
        Serial.printf("Failed to get MUTEX lock, skipping loop\n"); 
        return;
      }

      if(!currentState.isBound) {
        return;
      }

      byte convertedMode = convertHKModeToACMode(targetHeaterCoolerState->getVal());
      byte acMode = currentState.acMode;
      if(convertedMode != acMode) {
        switch(acMode) {
          case static_cast<byte>(FujiMode::COOL):
            targetHeaterCoolerState->setVal(static_cast<int>(HeatingCoolerState::Cool));
            break;
          case static_cast<byte>(FujiMode::HEAT):
            targetHeaterCoolerState->setVal(static_cast<int>(HeatingCoolerState::Heat));
            break;
          case static_cast<byte>(FujiMode::AUTO):
            targetHeaterCoolerState->setVal(static_cast<int>(HeatingCoolerState::HeatCool));
            break;
          case static_cast<byte>(FujiMode::FAN):
            break;
          case static_cast<byte>(FujiMode::DRY):
            break;
        }      

        if(acMode == static_cast<byte>(FujiMode::HEAT) || 
           acMode == static_cast<byte>(FujiMode::COOL) ||
           acMode == static_cast<byte>(FujiMode::AUTO)) {
               Serial.printf("loop(): Remote updated mode HKMODE: %d = ACMODE: %d -> ACMODE: %d\n", targetHeaterCoolerState->getVal(), convertedMode, acMode);
        }
      }

      if(currentTemp->getVal() != currentState.controllerTemp) {
        if(currentState.controllerTemp >= 0 && currentState.controllerTemp <= 100) {
          currentTemp->setVal(currentState.controllerTemp);
        }
      }

      byte temp = currentState.temperature;
      if(temp >= 16 && temp <= 30) {
        switch(acMode) {
          case static_cast<byte>(FujiMode::COOL):
              if(temp != coolingThresholdTemperature->getVal()) {
                coolingThresholdTemperature->setVal(temp);
              }
  
              if(currentTemp->getVal() > coolingThresholdTemperature->getVal()) {
                currentHeaterCoolerState->setVal(static_cast<int>(CurrentHeatingCoolerState::Cooling));
              } else {
                currentHeaterCoolerState->setVal(static_cast<int>(CurrentHeatingCoolerState::Idle));
              }  
              break;

          case static_cast<byte>(FujiMode::HEAT):
              if(temp != heatingThresholdTemperature->getVal()) {
                heatingThresholdTemperature->setVal(temp);
              }
  
              if(currentTemp->getVal() < heatingThresholdTemperature->getVal()) {
                currentHeaterCoolerState->setVal(static_cast<int>(CurrentHeatingCoolerState::Heating));
              } else {
                currentHeaterCoolerState->setVal(static_cast<int>(CurrentHeatingCoolerState::Idle));
              }
              break;

          case static_cast<byte>(FujiMode::AUTO):
              if(max(temp - 2, 16) != heatingThresholdTemperature->getVal()) {
                heatingThresholdTemperature->setVal(max(temp - 2, 16));
              }
  
              if(min(temp + 2, 30) != coolingThresholdTemperature->getVal()) {
                coolingThresholdTemperature->setVal(min(temp + 2, 30));
              }
  
              if(currentTemp->getVal() > coolingThresholdTemperature->getVal()) {
                currentHeaterCoolerState->setVal(static_cast<int>(CurrentHeatingCoolerState::Cooling));
              } else if(currentTemp->getVal() < heatingThresholdTemperature->getVal()) {
                currentHeaterCoolerState->setVal(static_cast<int>(CurrentHeatingCoolerState::Heating));
              } else {
                currentHeaterCoolerState->setVal(static_cast<int>(CurrentHeatingCoolerState::Idle));
              }
              break; 
        }
      }
    }


    boolean update(){
      bool inAutoMode = ( targetHeaterCoolerState->updated() && targetHeaterCoolerState->getNewVal() == static_cast<int>(HeatingCoolerState::HeatCool)) ||
                        (!targetHeaterCoolerState->updated() && targetHeaterCoolerState->getVal()    == static_cast<int>(HeatingCoolerState::HeatCool));
      bool inCoolMode = ( targetHeaterCoolerState->updated() && targetHeaterCoolerState->getNewVal() == static_cast<int>(HeatingCoolerState::Cool)) ||
                        (!targetHeaterCoolerState->updated() && targetHeaterCoolerState->getVal()    == static_cast<int>(HeatingCoolerState::Cool));
      bool inHeatMode = ( targetHeaterCoolerState->updated() && targetHeaterCoolerState->getNewVal() == static_cast<int>(HeatingCoolerState::Heat)) ||
                        (!targetHeaterCoolerState->updated() && targetHeaterCoolerState->getVal()    == static_cast<int>(HeatingCoolerState::Heat));
                        
      int heatThreshold = heatingThresholdTemperature->updated() ? heatingThresholdTemperature->getNewVal() : heatingThresholdTemperature->getVal();
      int coolThreshold = coolingThresholdTemperature->updated() ? coolingThresholdTemperature->getNewVal() : coolingThresholdTemperature->getVal();
      int midPoint = (heatThreshold - coolThreshold)/2 + coolThreshold;
      
      if(active->updated()) {
        if(active->getVal() != active->getNewVal()) {
          Serial.printf("update(): active %d -> %d\n", active->getVal(), active->getNewVal());
          
          if(xSemaphoreTake( pendingMutex, (TickType_t)200 ) == pdTRUE) {
            pendingFields |= kOnOffUpdateMask;
            pendingState.onOff = active->getNewVal() ? 1 : 0;
            fanControl.observePower(active->getNewVal() != 0);
            awaitingPowerEcho = true;
            powerRequestedAt = millis();
            xSemaphoreGive( pendingMutex);
          }

        }
      }

      if(targetHeaterCoolerState->updated())
      {
        if(targetHeaterCoolerState->getVal() != targetHeaterCoolerState->getNewVal()) {
          Serial.printf("update(): targetHeaterCoolerState %d -> %d\n", targetHeaterCoolerState->getVal(), targetHeaterCoolerState->getNewVal());

          if(xSemaphoreTake( pendingMutex, (TickType_t)200 ) == pdTRUE) {
            pendingFields |= kModeUpdateMask;
                
            switch(targetHeaterCoolerState->getNewVal()){
              case static_cast<int>(HeatingCoolerState::Cool):
                pendingState.acMode = static_cast<byte>(FujiMode::COOL);
                break;
              case static_cast<int>(HeatingCoolerState::Heat):
                pendingState.acMode = static_cast<byte>(FujiMode::HEAT);
                break;
              case static_cast<int>(HeatingCoolerState::HeatCool):
                pendingState.acMode = static_cast<byte>(FujiMode::AUTO);
                break;
            }
            xSemaphoreGive(pendingMutex);
          }
        }
      }

      if(coolingThresholdTemperature->updated() && inAutoMode) {
        if(coolingThresholdTemperature->getVal() != coolingThresholdTemperature->getNewVal())
        {
          Serial.printf("update(): coolingThresholdTemperature -> using midpoint %dC instead\n", midPoint);
          
          if(xSemaphoreTake(pendingMutex, (TickType_t) 200 ) == pdTRUE) {
            pendingFields |= kTempUpdateMask;
            pendingState.temperature = midPoint; // set to the midpoint
            xSemaphoreGive(pendingMutex);
          }
        }
      } else if(coolingThresholdTemperature->updated() && inCoolMode) {
        if(coolingThresholdTemperature->getVal() != coolingThresholdTemperature->getNewVal())
        {
          if(xSemaphoreTake(pendingMutex, (TickType_t) 200 ) == pdTRUE) {
            pendingFields |= kTempUpdateMask;
            pendingState.temperature = coolingThresholdTemperature->getNewVal(); // set to the midpoint
            xSemaphoreGive(pendingMutex);
          }
        }
      }

      if(heatingThresholdTemperature->updated() && inAutoMode) {
        if(heatingThresholdTemperature->getVal() != heatingThresholdTemperature->getNewVal())
        {
          Serial.printf("update(): heatingThresholdTemperature -> using midpoint %dC instead\n", midPoint);
          if(xSemaphoreTake(pendingMutex, (TickType_t) 200 ) == pdTRUE) {
            pendingFields |= kTempUpdateMask;
            pendingState.temperature = midPoint; // set to the midpoint
            xSemaphoreGive(pendingMutex);
          }
        }
      } else if(heatingThresholdTemperature->updated() && inHeatMode) {
        if(heatingThresholdTemperature->getVal() != heatingThresholdTemperature->getNewVal())
        {
          if(xSemaphoreTake(pendingMutex, (TickType_t) 200 ) == pdTRUE) {
            pendingFields |= kTempUpdateMask;
            pendingState.temperature = heatingThresholdTemperature->getNewVal(); // set to the midpoint
            xSemaphoreGive(pendingMutex);
          }
        }
      }

      return true;
      
    }
};


void FujiTaskLoop(void *pvParameters){
  FujiHeatPump hp;
  FujiStatus updateState;
  byte updatedFields = 0;

  Serial.print("FujiTask Begin on core ");
  Serial.println(xPortGetCoreID());

  Serial.print("Creating FujiHeatPump object\n");
  hp.connect(&Serial2, true, RX_PIN, TX_PIN);
    
  for(;;){
      if(xSemaphoreTake(pendingMutex, (TickType_t)200) == pdTRUE) {
        if(pendingFields) {
          memcpy(&updateState, &pendingState, sizeof(FujiStatus));
          updatedFields = pendingFields;
          pendingFields = 0;
        }
        xSemaphoreGive(pendingMutex);
      }

      // apply updated fields
      if(updatedFields & kOnOffUpdateMask) {
          hp.setOnOff(updateState.onOff == 1 ? true : false);
      }
      
      if(updatedFields & kTempUpdateMask) {
          hp.setTemp(updateState.temperature);
      }
      
      if(updatedFields & kModeUpdateMask) {
          hp.setMode(updateState.acMode);
      }
      
      if(updatedFields & kFanModeUpdateMask) {
          hp.setFanMode(updateState.fanMode);
      }
      
      if(updatedFields & kSwingModeUpdateMask) {
          hp.setSwingMode(updateState.swingMode);
      }
      
      if(updatedFields & kSwingStepUpdateMask) {
          hp.setSwingStep(updateState.swingStep);
      }

      updatedFields = 0;
  
      if(hp.waitForFrame()) {
        delay(60);
        hp.sendPendingFrame();

        updateState.isBound        = hp.isBound();;
        updateState.onOff          = hp.getCurrentState()->onOff;
        updateState.acMode         = hp.getCurrentState()->acMode;
        updateState.temperature    = hp.getCurrentState()->temperature;
        updateState.fanMode        = hp.getCurrentState()->fanMode;
        updateState.swingMode      = hp.getCurrentState()->swingMode;
        updateState.swingStep      = hp.getCurrentState()->onOff;
        updateState.controllerTemp = hp.getCurrentState()->controllerTemp;

        if(xSemaphoreTake(updateMutex, (TickType_t)200) == pdTRUE) {
          memcpy(&sharedState, &updateState, sizeof(FujiStatus)); //copy our local back to the shared
          xSemaphoreGive(updateMutex);
        }
      }

      hpIsBound = hp.isBound();
      
      delay(1);
  } 
}

void setup() {

  memset(sn, 0, 32);
  memset(acName, 0, 64);
    
  Serial.begin(115200);
  Serial.print("FujitsuHK startup\n");
  

  updateMutex = xSemaphoreCreateMutex();
  pendingMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(
                    FujiTaskLoop,   /* Task function. */
                    "FujiTask",     /* name of task. */
                    10000,          /* Stack size of task */
                    NULL,           /* parameter of the task */
                    19,             /* priority of the task */
                    &FujiTask,      /* Task handle to keep track of created task */
                    0);             /* pin task to core 0 */    

  Serial.print("HomeSpan begin on core ");
  Serial.println(xPortGetCoreID());
  
  homeSpan.setQRID("FUJI");
  homeSpan.setControlPin(BTN_PIN);
#if LED_PIN >= 0
  homeSpan.setStatusPixel(LED_PIN,COLOR_CYAN_HSV);
  homeSpan.setStatusAutoOff(30);
#endif
  homeSpan.setApSSID(acName);
  homeSpan.begin(Category::AirConditioners,"Fujitsu AirConditioner", "FUJIAC", "FUJIAC");

  sprintf(sn, "%08X", (uint32_t)ESP.getEfuseMac());
  sprintf(acName, "Fujitsu AirConditioner %s", sn);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name(acName);
      new Characteristic::Manufacturer("Fujitsu");
      new Characteristic::SerialNumber(sn);
      new Characteristic::Model("Fuji-HK");
      new Characteristic::FirmwareRevision("0.2");
      new Characteristic::Identify();
      
    new Service::HAPProtocolInformation();
      new Characteristic::Version("1.1.0");
      
    new FujitsuHK();
    new FujitsuFan();
    new FujitsuFanAuto();

  Serial.print("Finished setup\n");
}

void loop() {
  syncControlsFromBus();
  homeSpan.poll();
}
