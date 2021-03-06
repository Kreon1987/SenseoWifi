/*
SenseoWifi.cpp - base file for the SenseoWifi project.
Created by Thomas Dietrich, 2016-03-05.
Released under some license.
*/

#include <Homie.h>

#include "SenseoLed.h"
#include "SenseoSM.h"
#include "SenseoControl.h"
#include "Cup.h"
#include "pins.h"
#include "constants.h"
#include "testIO.cpp"

SenseoLed mySenseoLed(ocSenseLedPin);
SenseoSM mySenseoSM;
SenseoControl myControl(ocPressPowerPin, ocPressLeftPin, ocPressRightPin);
Cup myCup(cupDetectorPin);

HomieNode senseoNode("machine", "machine");
HomieSetting<bool> CupDetectorAvailableSetting("cupdetector", "Enable cup detection (TCRT5000)");
HomieSetting<bool> BuzzerSetting("buzzer", "Enable buzzer feedback (no water, cup finished, ...)");
HomieSetting<bool> RecipesActiveSetting("recipes", "Enable higher level recipes instead of direct control");

/**
* If a recipe is active, a certain process will be executed.
* BrewCup will power up, brew a cup and power off
* [0 = off, 1 = 1cup, 2= 2cup]
*/
int recipeBrewCups = 0;

/**
* Called by the LED changed interrupt
*/
void ledChangedHandler() {
  mySenseoLed.pinStateToggled();
}

/**
* Called by Homie upon an MQTT message to '.../power'
* No MQTT response is sent from this routine, as pessimistic feedback will be handled in the state machine.
*/
bool powerHandler(const HomieRange& range, const String& value) {
  if (value != "ON" && value !="OFF" && value != "RESET") {
    senseoNode.setProperty("debug").setRetained(false).send("power: malformed message content. Allowed: [ON,OFF].");
    return false;
  }

  if (value == "ON" && mySenseoSM.getState() == SENSEO_OFF) {
    myControl.pressPowerButton();
  }
  else if (value == "OFF" && mySenseoSM.getState() != SENSEO_OFF) {
    myControl.pressPowerButton();
  }
  else if (value == "RESET") {
    tone(beeperPin, 4096, 8000);
    Homie.reset();
  }
  return true;
}

/**
* Called by Homie upon an MQTT message to '.../brew'.
* No MQTT response is sent from this routine, as pessimistic feedback will be handled in the state machine.
*/
bool brewHandler(const HomieRange& range, const String& value) {
  /**
  * Catch incorrect messages
  */
  if (value != "1cup" && value !="2cup") {
    senseoNode.setProperty("debug").setRetained(false).send("brew: malformed message content. Allowed: [1cup,2cup].");
    return false;
  }

  /**
  * Catch missing or full cup
  */
  if (CupDetectorAvailableSetting.get()) {
    if (myCup.isNotAvailable() || myCup.isFull()) {
      senseoNode.setProperty("debug").setRetained(false).send("brew: no or full cup present. Not executing.");
      return false;
    }
  }

  /**
  * Catch already enqueued recipe
  * Ensures only one recipe is active at all times
  */
  if (recipeBrewCups != 0) {
    senseoNode.setProperty("recipe").setRetained(false).send("IN_PROGRESS");
    senseoNode.setProperty("debug").setRetained(false).send("brew: recipe already enqueued, takes priority.");
    return false;
  }

  /**
  * Catch wrong senseo state machine state
  * Enqueue recipe
  */
  if (mySenseoSM.getState() != SENSEO_READY) {
    senseoNode.setProperty("debug").setRetained(false).send("brew: machine currently in the wrong state (not ready).");
    if (RecipesActiveSetting.get()) {
      if (value == "1cup") recipeBrewCups = 1;
      if (value == "2cup") recipeBrewCups = 2;
      senseoNode.setProperty("debug").setRetained(false).send("recipe: Powering up. Waiting for SENSEO_READY state.");
      senseoNode.setProperty("recipe").setRetained(false).send("RECEIVED");
      myControl.pressPowerButton();
      return true;
    }
    return false;
  }

  /**
  * If all the above didn't trap - execute
  */
  if (value == "1cup") myControl.pressLeftButton();
  if (value == "2cup") myControl.pressRightButton();
  return true;
}

/**
* Senseo state machine, transition reaction: exit actions
*/
void senseoStateExitAction() {
  switch (mySenseoSM.getStatePrev()) {
    case SENSEO_OFF: {
      senseoNode.setProperty("power").send("ON");
      senseoNode.setProperty("outOfWater").send("false");
      senseoNode.setProperty("brew").send("false");
      break;
    }
    case SENSEO_HEATING: {
      break;
    }
    case SENSEO_READY: {
      break;
    }
    case SENSEO_BREWING: {
      senseoNode.setProperty("brew").send("false");
      // Determine brewed cup size based on time in brewing state
      int brewedSeconds = mySenseoSM.getSecondsInLastState();
      int brewedSize = 0;

      if (brewedSeconds <= 10) break;

      // 0---------------------|-----+-----|-----+-----|-------100
      int tolerance = (BrewHeat2CupSeconds - BrewHeat1CupSeconds) / 2;

      if (mySenseoSM.getState() == SENSEO_READY) {
        if (abs(brewedSeconds - BrewHeat1CupSeconds) < tolerance) {
          brewedSize = 1;
        }
        else if (abs(brewedSeconds - BrewHeat2CupSeconds) < tolerance) {
          brewedSize = 2;
        }
      }

      tolerance = (Brew2CupSeconds - Brew1CupSeconds) / 2;
      if (mySenseoSM.getState() == SENSEO_NOWATER || mySenseoSM.getState() == SENSEO_OFF) {
        if (abs(brewedSeconds - Brew1CupSeconds) < tolerance) {
          brewedSize = 1;
        }
        else if (abs(brewedSeconds - Brew2CupSeconds) < tolerance) {
          brewedSize = 2;
        }
      }

      senseoNode.setProperty("brewedSize").setRetained(false).send(String(brewedSize));
      senseoNode.setProperty("debug").setRetained(false).send(String("brew: ") + String(brewedSeconds) + String(" seconds"));
      if (brewedSize == 0) {
        senseoNode.setProperty("debug").setRetained(false).send("brew: unexpected time in SENSEO_BREWING state. Please adapt timings.");
      }

      if (CupDetectorAvailableSetting.get()) myCup.fillUp();

      // If recipe is active, transition to the last step "power off"
      //TODO Check if brewedSize == recipeBrewCups ?
      if (recipeBrewCups != 0) {
        recipeBrewCups = 0;
        senseoNode.setProperty("recipe").setRetained(false).send("FINISHED");
        senseoNode.setProperty("debug").setRetained(false).send("recipe: SENSEO_OFF state reached. Finished.");
        myControl.pressPowerButton();
      }
      break;
    }
    case SENSEO_NOWATER: {
      if (mySenseoSM.getState() != SENSEO_OFF) senseoNode.setProperty("outOfWater").send("false");
      break;
    }
  }
}

/**
* Senseo state machine, transition reaction: entry actions
*/
void senseoStateEntryAction() {
  switch (mySenseoSM.getState()) {
    case SENSEO_OFF: {
      senseoNode.setProperty("power").send("OFF");
      /** Cancel recipe if set */
      if (recipeBrewCups != 0) {
        recipeBrewCups = 0;
        senseoNode.setProperty("recipe").setRetained(false).send("CANCELED");
        senseoNode.setProperty("debug").setRetained(false).send("recipe: SENSEO_OFF state reached. Cancelling.");
      }
      break;
    }
    case SENSEO_HEATING: {
      break;
    }
    case SENSEO_READY: {
      if (BuzzerSetting.get()) tone(beeperPin, 1536, 500);
      /** Execute recipe if set */
      if (recipeBrewCups != 0) {
        if (!CupDetectorAvailableSetting.get() || (CupDetectorAvailableSetting.get() && myCup.isAvailable() && myCup.isEmpty())) {
          if (recipeBrewCups == 1) myControl.pressLeftButton();
          if (recipeBrewCups == 2) myControl.pressRightButton();
          senseoNode.setProperty("debug").setRetained(false).send("recipe: SENSEO_READY state reached, executing.");
        } else {
          /** If no or full cup available, this should not happen at this point of a recipe */
          recipeBrewCups = 0;
          senseoNode.setProperty("recipe").setRetained(false).send("CANCELED");
          senseoNode.setProperty("debug").setRetained(false).send("recipe: SENSEO_READY state reached but no or full cup found. Cancelling.");
        }
      }
      break;
    }
    case SENSEO_BREWING: {
      senseoNode.setProperty("brew").send("true");
      break;
    }
    case SENSEO_NOWATER: {
      if (BuzzerSetting.get()) tone(beeperPin, 4096, 2000);
      senseoNode.setProperty("outOfWater").send("true");

      /** Delete recipe if set */
      if (recipeBrewCups != 0) {
        recipeBrewCups = 0;
        senseoNode.setProperty("recipe").setRetained(false).send("CANCELED");
        senseoNode.setProperty("debug").setRetained(false).send("recipe: SENSEO_NOWATER state reached. Cancelling.");
      }
      break;
    }
  }
}

/**
*
*/
void setupHandler() {
  if (BuzzerSetting.get()) tone(beeperPin, 2048, 500);

  attachInterrupt(digitalPinToInterrupt(ocSenseLedPin), ledChangedHandler, CHANGE);

  Homie.getLogger() << endl << "☕☕☕☕ Enjoy your SenseoWifi ☕☕☕☕" << endl << endl;

  /**
  * Send status data once.
  */
  senseoNode.setProperty("opState").send(mySenseoSM.getStateAsString());
  if (CupDetectorAvailableSetting.get()) {
    senseoNode.setProperty("cupAvailable").send(myCup.isAvailable() ? "true" : "false");
    senseoNode.setProperty("cupFull").send(myCup.isFull() ? "true" : "false");
  }
  senseoNode.setProperty("outOfWater").send("false");
  senseoNode.setProperty("brew").send("false");
}

/**
*
*/
void loopHandler() {
  /**
  * Check and update the cup availability, based on the cup detector signal.
  * (no cup, cup available, cup full)
  */
  if (CupDetectorAvailableSetting.get()) {
    myCup.updateState();
    if (myCup.isAvailableChanged()) {
      senseoNode.setProperty("cupAvailable").send(myCup.isAvailable() ? "true" : "false");
    }
    if (myCup.isFullChanged()) {
      senseoNode.setProperty("cupFull").send(myCup.isFull() ? "true" : "false");
    }
  }

  /**
  * Update the low level LED state machine based on the measured LED timings.
  * (off, slow blinking, fast blinking, on)
  */
  mySenseoLed.updateState();
  if (mySenseoLed.hasChanged()) {
    if (true) Homie.getLogger() << "LED state machine, new LED state: " << mySenseoLed.getStateAsString() << endl;
  }

  /**
  * Update the higher level Senseo state machine based on the LED state.
  * (off, heating, ready, brewing, no water)
  */
  mySenseoSM.updateState(mySenseoLed.getState());
  if (mySenseoSM.stateHasChanged()) {
    if (true) Homie.getLogger() << "Senseo state machine, new Senseo state: " << mySenseoSM.getStateAsString() << endl;
    senseoNode.setProperty("opState").send(mySenseoSM.getStateAsString());

    senseoStateExitAction();
    senseoStateEntryAction();
  }

  /**
  * Non-blocking Low-High-Low transition.
  * Check for a simulated button press - release after > 100ms
  */
  myControl.releaseIfPressed();
}

void setup() {
  Serial.begin(115200);

  /**
  * pin initializations
  */
  pinMode(ocPressLeftPin, OUTPUT);
  pinMode(ocPressRightPin, OUTPUT);
  pinMode(ocPressPowerPin, OUTPUT);
  pinMode(ocSenseLedPin, INPUT_PULLUP);

  digitalWrite(ocPressPowerPin, LOW);
  digitalWrite(ocPressLeftPin, LOW);
  digitalWrite(ocPressRightPin, LOW);

  pinMode(beeperPin, OUTPUT);
  pinMode(resetButtonPin, INPUT_PULLUP);

  if (CupDetectorAvailableSetting.get()) {
    pinMode(cupDetectorPin, INPUT_PULLUP);
    pinMode(cupDetectorAnalogPin, INPUT);
  }

  /**
  * Testing routine. Activate only in development environemt.
  * Tests the circuit and Senseo connections, loops indefinitely.
  *
  * Wifi will NOT BE AVAILABLE, no OTA!
  */
  if (false) testIO();


  /**
  * Homie specific settings
  */
  Homie_setFirmware("senseo-wifi-wemos", senseoWifiFWVersion);
  Homie_setBrand("SenseoWifi");
  //Homie.disableResetTrigger();
  Homie.disableLedFeedback();
  Homie.setResetTrigger(resetButtonPin, LOW, 5000);
  Homie.setSetupFunction(setupHandler);
  Homie.setLoopFunction(loopHandler);

  /**
  * Homie: Options, see at the top of this file.
  */
  CupDetectorAvailableSetting.setDefaultValue(true);
  BuzzerSetting.setDefaultValue(true);
  RecipesActiveSetting.setDefaultValue(true);

  /**
  * Homie: Advertise custom SenseoWifi MQTT topics
  */
  senseoNode.advertise("debug"); // not retained
  senseoNode.advertise("opState");
  senseoNode.advertise("power").settable(powerHandler);
  senseoNode.advertise("brew").settable(brewHandler);
  senseoNode.advertise("brewedSize"); // not retained
  senseoNode.advertise("outOfWater");
  senseoNode.advertise("recipe"); // not retained
  if (CupDetectorAvailableSetting.get()) senseoNode.advertise("cupAvailable");
  if (CupDetectorAvailableSetting.get()) senseoNode.advertise("cupFull");

  if (BuzzerSetting.get()) tone(beeperPin, 1536, 2000);

  Homie.setup();
}

void loop() {
  Homie.loop();
}
