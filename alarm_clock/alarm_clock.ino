
/* CLOCK v4
   Russ Winch
   Started February 2016

   TODO:

   Hardware
    replace the arduino with ATMEGA chip and required accessories
    usb power - any electrical considerations? polyfuse?
    mounting of the usb power socket
    jumper to select between usb power or ICSP
    include a switch or jumper to disable sound?
    change the LED display to one unit?
    led display isn't as bright as it could be - resistor mathmatics required...
    protect the display with clear or slighly smoked acrylic (brightness!!)

   Software
    improve alarm tones - use alarmStarted time to change the tone / modes?
    **automatically exit settings mode if no buttons pressed for a while? **DONE, TEST
    implement pointers
    look for unused variables and remove
    **implement snooze extension DONE, TEST
    convert changedMode into a function
   Low priority
    add negative for temperature?
    settings mode for alarm tones?
    settings mode for 12/24h mode?
    implement ? syntax..  x = (y > 1) ? 10 : 20   //if (y > 1) x = 10; else x = 20;
    implement transitions between display modes **develop this further/properly
    improve LED decode function to use binary settings
    implement scrolling dps during seconds display

   Bugs
    confirmFlash continues even when mode is changed
    changed mode detection resets if another button released - eg pressing al when still holding snooze
*/


#define DS3231 0x68 //I2C address of DS3231
#include <Wire.h> //for I2C communication
#include <EEPROM.h> //for saving alarm and settings to EEPROM

//input pins
const int alarmSw0Pin = A0; //constant alarm switch
const int alarmSw1Pin = A1; //momentary alarm switch
const int snoozeSwPin = 4; //momentary snooze switch

//output pins
//A4 & A5 reserved for I2C Clock and Data, respectively
const int piezoPin = 3; //piezo sounder
const int snoozeLedPin = 5; //snooze led
const int ledEnable = 6; //MC14489B led driver enable
const int ledClock = 7; //MC14489B led driver clock
const int ledData = 8; //MC14489B led driver data

//mode variables
int mode = 0; //main mode selector
int oldMode = 0; //mode detection
boolean changedModeFlag = false; //flag to detect if mode has changed
int setMode = 1; //settings mode selector
byte currentSetting[2]; //holds current variables when settings entered
byte currentSettingMax[2] = {59, 23}; //max mins and hours allowed
unsigned long lastSet = 0; //time the last button was pressed to adjust a setting
int setTarget = 0; //target for settings [0=clock, 1=alarm]
int displayFor = 3500; //time to display alarm before returning to clock and flash LEDs when confirming setting. keep above longHoldDelay
unsigned long settingsTimeout = 0;

//clock variables
byte clockData[3]; //seconds, minutes, hours
byte alarm1Data[3]; //seconds, minutes, hours
byte alarm2Data[2]; //minutes, hours
byte tempData[2]; //integer, decimal
byte statusData; //status register

//temperature variables
float temperature[3]; //the current temperature [0=sign, 1=integer, 2=decimal]
float currentTemp;
const int tempLogReadings = 10; //number of readings to average
float tempLog[tempLogReadings]; //readings for average
int tempLogIndex = 0; //index for average
float tempLogTotal = 0; //total for average
int oldSeconds = 0; //to check if the clock has incremented

//led driver variables
byte ledDisplayByte[3]; //led display data to write back [0=D0-D7, 1=D8-D15, 2=D16-D23]
byte oldLedByte[4]; //data from the last update
int ledDecode = 0; //decode mode - used to turn off banks

//led variables
int fadeValue = 0;
boolean fadeDir = false; //direction of fade
boolean alarmLed = false; //alarm LED flag
boolean flashDots = false; //flash seperator leds
boolean confirmFlash = false; //overall confirm flag
unsigned long confirmFlashTime; //time led flashing started when confirming setting

//button variables
const int totalButtons = 3; //total connected buttons to check
boolean buttonData[totalButtons]; //current button reading
boolean oldButtonData[totalButtons] = {LOW, HIGH, HIGH}; //previous button reading
int buttonState[totalButtons]; //current button state [0=alSw0, 1= alSw1, 2=snSw]
int oldButtonState[totalButtons]; //previous button state
unsigned long bounceTime[totalButtons]; //millis() times for each button last pressed for debounce
unsigned long holdTime[totalButtons];
unsigned long incTime = 0; //time since last increment
int bounceDelay = 40; //debounce time
int holdDelay = 1000; //time to wait for button hold
int longHoldDelay = 3000; //time to wait for long button hold
int incDelay = 80; //time to wait between incrementing during button hold - speed

//alarm variables
const int totalAlarms = 3; //total software alarms
int currentAlarm = 99; //currently set alarm. set to 99 as a default, to be changed during setup()
int displayedAlarm = 0; //currently displayed alarm
int alarmTime[totalAlarms][2]; //array to hold alarm times [mins, hours]
boolean alarmStatus[2]; //holds the alarm triggered flags [0=AL1, 1=AL2]
boolean alarmSet[2]; //holds the alarm set flags [0=AL1, 1=AL2]
boolean alarmTriggered = false;
int defaultSnooze; //snooze time in mins - will read from EEPROM
int setSnooze; //to store defaultSnooze while setting
int maxSnooze = 20; //maximum settable snoozetime
unsigned long alarmStarted; //time alarm started sounding
unsigned long alarmStop = 10 * 60000; //alarm to stop itself after 10 mins
int snoozeLeft; //mins

//alarm tones
boolean toneActive = false; //is there an active tone
unsigned long toneStarted = 0; ///millis when tone started
int toneDuration = 0; //tone duration
int toneDelay = 0; //delay between tones
int toneReps = 0; //cycle through the reps for the sound
int t; //tone frequency variable

//EEPROM
const int eepromStart = 10; //address in eeprom to store alarms from
const int eepromSnooze = 50; //address for snooze time setting

void setup() {
  //inputs
  pinMode(alarmSw0Pin, INPUT_PULLUP);
  pinMode(alarmSw1Pin, INPUT_PULLUP);
  pinMode(snoozeSwPin, INPUT_PULLUP);

  //outputs
  pinMode(snoozeLedPin, OUTPUT); //snooze led
  pinMode(ledEnable, OUTPUT); //for the led driver
  pinMode(ledClock, OUTPUT); //for the led driver
  pinMode(ledData, OUTPUT); //for the led driver
  //  pinMode(piezoPin, OUTPUT); //tone appears to negate the need for this

  //initiate wire
  Wire.begin();

  //update alarms
  defaultSnooze = EEPROM.read(eepromSnooze); //retrieve defaultSnooze setting

  getAlarm2();
  //read from EEPROM and determine currentAlarm
  for (int e = 0; e < totalAlarms; e++) {
    alarmTime[e][0] = EEPROM.read(e + eepromStart); //mins
    alarmTime[e][1] = EEPROM.read(e + eepromStart + totalAlarms); //hours
    if (alarmTime[e][0] == bcdToDec(alarm2Data[0] & B01111111) && alarmTime[e][1] == bcdToDec(alarm2Data[1] & B00111111)) { //if current alarm is set in DS3231
      currentAlarm = e;
    }
  }
  if (currentAlarm == 99) { //if alarm couldn't be determined - eg battery backup removed
    currentAlarm = 0; //choose alarm 0
    displayedAlarm = 0; //needed for setAlarmEeprom
    setAlarm2(alarmTime[0][1], alarmTime[0][0]); //set to alarm 0 (hours, mins)
  }

  resetAlarms(); //so alarm can't trigger on startup

  //debugging stuff
//        Serial.begin(9600);
  //  setTime(0,3,10); //Hours,Mins,Secs
  //  setAlarm1(8,30);
  //  setAlarm2(7,30);

  //  for (int s = 0; s<51;s++){
  //    Serial.print(s);
  //    Serial.print(": ");
  //    Serial.println(EEPROM.read(s));
  //  }

}

void loop() {
  getButtons();
  oldMode = mode;

  //whilst in a display mode
  if (mode < 4) {
    if (buttonState[2] == 2) { //if snooze button released
      if (alarmSet[0]) { //if snoozing
        mode = 4; //show snoozetime
//        //changedMode = true;
        lastSet = millis();
      } else {
        mode++; //next display mode
        if (mode > 3) mode = 0; //loop round
      }
    } 
    if (buttonState[1] == 4 && alarmSet[0]) { //if al2 switch long held
      resetAlarms();
    }
    if (buttonState[1] == 2) { //if al2 switch released
      if (alarmSet[0]) mode = 4; //if snoozing - show snoozetime
      
      else {
//        printTransition(); // testing        
        mode = 5; //else show alarm
        displayedAlarm = currentAlarm;

      }
      //changedMode = true;
      lastSet = millis();
    }
  }

  //if showing time and snooze switch long held - go to settings
  if (mode == 0 && buttonState[2] == 4 && !alarmTriggered) {
    currentSetting[0] = bcdToDec(clockData[1]); //mins
    currentSetting[1] = bcdToDec(clockData[2]); //hrs
    setTarget = 0; //set clock
    setMode = 1; //reset to setting hours first
    mode = 6; //show settings
    settingsTimeout = millis();
    //changedMode = true;
  }

//extend snooze by 5 mins and display...
  if (mode == 4 && (buttonState[2] == 2) && !changedMode()) { //if showing snoozetime
      snooze(true); //extend
      lastSet = millis();
  }

  //when alarm displayed
  if (mode == 5) {
    if (buttonState[2] == 1 || buttonState[1] == 1) lastSet = millis(); //if either switch pressed, give some extra time to display

    if (buttonState[1] == 4 && !changedMode() && displayedAlarm != currentAlarm) { //if alarm switch long held and not setting current alarm to itself
      setAlarm2(alarmTime[displayedAlarm][1], alarmTime[displayedAlarm][0]); //hrs, mins
      currentAlarm = displayedAlarm;
      setTarget = 1; //writing to alarm
      confirm(true); //do some confirming
      changedModeFlag = true; //changedMode detection required to ensure writing isn't continuous when holding button (EEPROM!!)
      lastSet = millis(); //start counter for timeout
    }
    if (buttonState[2] == 4) { //if snooze switch long held
      currentSetting[0] = alarmTime[displayedAlarm][0]; //mins
      currentSetting[1] = alarmTime[displayedAlarm][1]; //hrs
      setTarget = 1; //set alarm
      mode = 6; //go to settings
      setMode = 1; //reset to setting hours first
//      changedModeFlag = true;
      lastSet = millis();
      settingsTimeout = millis(); //reset timeout      
    }
    if (buttonState[1] == 2 && !changedMode()) { //if al2 switch released and not just switched to settings
      lastSet = millis();
      displayedAlarm++; //show next alarm
      if (displayedAlarm == totalAlarms) displayedAlarm = 0; //loop back to first alarm
    }
  }

  //if in settings mode (and not still holding button from entering settings mode)
  if (mode == 6 && !changedMode()) {
    if (buttonState[2] == 1 || ((buttonState[2] >= 3) && (millis() - incTime > incDelay))) { //if snooze switch pressed or held or longheld and delay since last increment
      currentSetting[setMode]++; //increment current setting
      if (currentSetting[setMode] > currentSettingMax[setMode]) currentSetting[setMode] = 0; //wrap current setting
      lastSet = millis(); //set time since last set - so they start flashing after delay
      incTime = millis(); //set time since last increment - to prevent settings changing too quickly
      settingsTimeout = millis(); //reset timeout

    } else if (buttonState[1] == 2) { //if al2 switch released. else used incase both buttons held together
      setMode--; //goto next setting
      settingsTimeout = millis(); //reset timeout
      if (setMode < 0) { //if past end of settings - save and exit
        confirm(true);
        resetAlarms(); //so alarm doesn't trigger instantly
        switch (setTarget) { //determines target for settings
          case 0: //clock
            setTime(currentSetting[1], currentSetting[0], 0); //hours, mins, secs
            mode = 0;
            break;
          case 1: //alarm
            setAlarm2(currentSetting[1], currentSetting[0]); //write to DS3231 (hours, mins)
            alarmTime[displayedAlarm][0] = currentSetting[0]; //update alarm mins
            alarmTime[displayedAlarm][1] = currentSetting[1]; //update alarm hrs
            currentAlarm = displayedAlarm; //update current alarm
            mode = 5; //display alarm
            lastSet = millis(); //save time of setting so alarm can be displayed for a short while
            break;
        }
      }
    }
  }

  //if in print snooze mode
  if (mode == 3) {
    if (buttonState[2] == 4) { //if snooze switch long held
      setSnooze = defaultSnooze; //store current snoozetime
      mode = 7; //go to snooze settings
      settingsTimeout = millis();
      //changedMode = true;
    }
  }

  //if in snooze settings mode
  if (mode == 7 && !changedMode()) {
    if (buttonState[2] == 1 || ((buttonState[2] >= 3) && (millis() - incTime > incDelay))) { //if snooze switch pressed or held or longheld and delay since last increment
      setSnooze ++; //increment current setting
      if (setSnooze > maxSnooze) setSnooze = 1; //roll over max
      lastSet = millis(); //set time since last set - so they start flashing after delay
      incTime = millis(); //set time since last increment - to prevent settings changing too quickly
      settingsTimeout = millis(); //reset timeout

    } else if (buttonState[1] == 2) { //if al2 switch released. else used incase both buttons held together
      defaultSnooze = setSnooze; //update defaultSnooze
      EEPROM.update(eepromSnooze, defaultSnooze); //save setting to EEPROM
      mode = 3; //change to view mode
      confirm(true);
      setTarget = 2; //setting snooze
    }
  }

 

  //check alarms
  if (((alarmStatus[0] && alarmSet[0]) || (alarmStatus[1] && alarmSet[1])) && !alarmTriggered) {
    alarmTriggered = true;
    alarmStarted = millis();
    resetFade(); // start fading leds from 0
    mode = 0; //return to clock
  }

  //if alarm currently triggered
  if (alarmTriggered) {
    alarmSounds(3);
    if (buttonState[1] == 2) { //if alarm momentary switch pressed when alarm triggered - reset
      resetAlarms();
      mode = 0; //show time
    }
    if (buttonState[2] == 2) {
      snooze(false); //if snooze switch released - snooze
      mode = 4; //show snoozetime
      lastSet = millis();
    }
  } else alarmSounds(0);

  
  //alarm switch
  if (buttonState[0] == 0) { //if al1 switch on
    if (!alarmSet[1] && alarmStatus[1]) resetAlarms(); //if just switched on and alarm previously triggered
    alarmSet[1] = true; //turn on alarm
    //    alarmSounds(1); //testing

  } else { //if alarm switch off
    alarmSet[1] = false; //deactivate alarms
    if (alarmTriggered || alarmSet[0]) { //if alarm triggered or snoozing
      resetAlarms();
      if (mode == 4) mode = 0; //if showing snoozetime - return to clock
    }
    //    alarmSounds(0); //testing
  }


  //if alarm or snooze displayed without button press, timeout and return to clock
  if ((mode == 4 || mode == 5) && (millis() - lastSet > displayFor)) mode = 0;
  
  //if alarm switch toggled off and currently in settings - exit setting
  if ((mode == 6 || mode == 7) && buttonState[0] == 1 && oldButtonState[0] == 0) mode = 0;

  //if currently in settings and no button pressed for a while - exit setting
  if ((mode == 6 || mode == 7) && millis() - settingsTimeout > 30000) mode = 0; // 

  //if alarm has sounded without cancel for too long
  if (alarmTriggered && millis() - alarmStarted > alarmStop) resetAlarms();

  //when confirm flash has finished - reset
  if (millis() - confirmFlashTime > displayFor) confirm(false);

  getTime();
  getTemp();
  getAlarm1();
  getAlarm2();
  getAlarmStatus();
  updateLeds();

  //main mode
  switch (mode) {
    case 0:
      printTime();
      break;
    case 1:
      printSeconds();
      break;
    case 2:
      printTemp();
      break;
    case 3:
      printSnooze(false); //show snooze setting - display only
      break;
    case 4:
      printSnoozeRemain(); //show time left whilst snoozing
      break;
    case 5:
      printAlarm(displayedAlarm);
      break;
    case 6:
      printSetting();
      break;
    case 7:
      printSnooze(true); //show snooze setting and set
      break;
  }

  //check for changes to display since last refresh  (warning in datasheet about potential dim segments with continuous writing of same data)
  int a = 0;
  for (int c = 0; c < 3; c++) { //check for changes since last refresh
    if (oldLedByte[c] != ledDisplayByte[c]) a++; //if any display bytes have changed
  }
  if (oldLedByte[3] != ledDecode) a++; //plus, if any changes required to control byte
  if (a > 0) updateDisplay(); //if update required - update
}

//convert BCD to decimal
byte bcdToDec (byte val) {
  return (10 * ((val & B11110000) >> 4)) + (val & B00001111);
}

//convert decimal to BCD
byte decToBcd (byte val) {
  return ((val / 10) << 4) | (val % 10);
}

//detect changed mode
boolean changedMode() {
  if (mode != oldMode) changedModeFlag = true;
  return changedModeFlag;
}



//check if seconds is even - for flashing of various items **test ? syntax
boolean even() {
  return (clockData[0] % 2) ? true : false; //if seconds are even
//  else return false;
}

//deci-even - for fast flashing of various items
boolean dEven() {
  if ((millis() / 100) % 2) return true; //if millis() / 100
  else return false;
}

//add 12hour mode?
void setTime(int hrs, int mins, int secs) { //clock/alarm [0=clock, 1/2=alarm], seconds, minutes, hours
  Wire.beginTransmission(DS3231);
  Wire.write(0x00); //start at byte 0
  Wire.write(decToBcd(secs)); //seconds
  Wire.write(decToBcd(mins)); //minutes
  Wire.write(decToBcd(hrs)); //hours and set 24h format
  //  Wire.write(B01000000 | decToBcd(11)); //hours to 11AM, and set 12h format ** not tested, needs indicator setting
  Wire.endTransmission();
}

void setAlarm1(int hours, int mins, int secs) {
  Wire.beginTransmission(DS3231);
  Wire.write(0x07); //start at byte 7
  Wire.write(decToBcd(secs)); //seconds
  Wire.write(decToBcd(mins)); //minutes
  Wire.write(decToBcd(hours)); //hours
  Wire.write(B10000000); //masked A1M4 so alarm looks for match on hours, mins and secs
  Wire.endTransmission();
}

void setAlarm2(int hours, int mins) {
  Wire.beginTransmission(DS3231);
  Wire.write(0x0B); //start at byte 11
  Wire.write(decToBcd(mins)); //minutes
  Wire.write(decToBcd(hours)); //hours
  Wire.write(B10000000); //masked A2M4 so alarm looks for match on hours and mins
  //  Wire.write(B01000000 | day); //day of week, masked DY/DT so alarm looks for match on dayofweek, hours, mins
  Wire.endTransmission();
  setAlarmEeprom(mins, hours, displayedAlarm); //save to EEPROM
}

void resetAlarms() {
  getAlarmStatus(); //read status
  Wire.beginTransmission(DS3231);
  Wire.write(0x0F); //start at byte 15
  Wire.write(statusData & B11111100); //write back status with alarm flags reset
  Wire.endTransmission();
  alarmTriggered = false;
  alarmSet[0] = false; //turn off snooze
}

void snooze(boolean extend) {
  if (alarmTriggered) resetAlarms();
  int snoozeFor;
  if (extend) {
    snoozeLeft = bcdToDec((alarm1Data[1] & B01111111)) - bcdToDec(clockData[1]); //mins
    if (snoozeLeft < 0) snoozeLeft += 60; //rollover if necessary
    snoozeFor = ((snoozeLeft/5)*5) + 5;
    if (snoozeFor > 55) snoozeFor = 5; //rollover
  } else snoozeFor = defaultSnooze;

  int snoozeAlarm[3] = { bcdToDec(clockData[0]), bcdToDec(clockData[1]) + snoozeFor, bcdToDec(clockData[2]) }; //secs, mins,hours
  if (snoozeAlarm[1] >= 60) { //mins rollover
    snoozeAlarm[1] -= 60;
    snoozeAlarm[2]++;
    if (snoozeAlarm[2] == 24) { //hours rollover
      snoozeAlarm[2] = 0;
    }
  }
  setAlarm1(snoozeAlarm[2], snoozeAlarm[1], snoozeAlarm[0]); //set snooze alarm to Al1
  alarmSet[0] = true; //turn on snooze
}

void setAlarmEeprom (int m, int h, int al) { //mins, hours, alarm target
  EEPROM.update(al + eepromStart, m); //mins
  EEPROM.update(al + eepromStart + totalAlarms, h); //hours
}

void getTime() {
  Wire.beginTransmission(DS3231);
  Wire.write(0x00); //read from byte 0 (seconds)
  Wire.endTransmission();
  Wire.requestFrom(DS3231, 3); //request 3 bytes - seconds, minutes, hours
  for (int i = 0; i < 3; i++) {
    clockData[i] = Wire.read();
  }
}

void getAlarm1() {
  Wire.beginTransmission(DS3231);
  Wire.write(0x07); //read from byte 7 (alarm1 seconds)
  Wire.endTransmission();
  Wire.requestFrom(DS3231, 3); //request 3 bytes - seconds, minutes, hours
  for (int i = 0; i < 3; i++) {
    alarm1Data[i] = Wire.read();
  }
}

void getAlarm2() {
  Wire.beginTransmission(DS3231);
  Wire.write(0x0B); //read from byte 11 (alarm2 minutes)
  Wire.endTransmission();
  Wire.requestFrom(DS3231, 2); //request 2 bytes - minutes, hours
  for (int i = 0; i < 2; i++) {
    alarm2Data[i] = Wire.read();
  }
}

void getAlarmStatus() {
  Wire.beginTransmission(DS3231);
  Wire.write(0x0F); //read from byte 15
  Wire.endTransmission();
  Wire.requestFrom(DS3231, 1); //request 1 byte
  statusData = Wire.read();
  alarmStatus[0] = statusData & B00000001; //alarm1 flag
  alarmStatus[1] = (statusData & B00000010) >> 1; //alarm2 flag
}

void getTemp() {
  Wire.beginTransmission(DS3231);
  Wire.write(0x11); //read from byte 17
  Wire.endTransmission();
  Wire.requestFrom(DS3231, 2); //request 2 bytes
  tempData[0] = Wire.read(); //sign, integer
  tempData[1] = Wire.read() >> 6; //decimal, shift right as only bytes 7 and 8 are used, +-0.25 accuracy
  if (oldSeconds != clockData[0]) { //update if seconds have changed. 1 reading per second refresh rate
    tempLogTotal -= tempLog[tempLogIndex]; //remove old data
    temperature[0] = (tempData[0] & B10000000) >> 7; //sign. 1=negative
    temperature[1] = tempData[0] & B01111111; //integer
    temperature[2] = float(tempData[1] * 0.25); //decimal, in .25 increments
    tempLog[tempLogIndex] = pow(-1, temperature[0]) * (temperature[1] + temperature[2]); //add data
    tempLogTotal += tempLog[tempLogIndex]; //add data to total
    tempLogIndex++; //advance array pointer
    if (tempLogIndex >= tempLogReadings) tempLogIndex = 0; //loop array pointer

    if (millis() < 10000) { //if just started up (and for 10 seconds every ~50 days) lazy coding :o|
      currentTemp = float(round((pow(-1, temperature[0]) * (temperature[1] + temperature[2])) * 2)) / 2; //last reading and round to .5 precision, because array for average will not yet be filled
    } else currentTemp = float(round((tempLogTotal / float(tempLogReadings)) * 2)) / 2; //calculate new average and round
    oldSeconds = clockData[0]; //update oldSeconds with new data
  }
}

void getButtons() {
  buttonData[0] = digitalRead(alarmSw0Pin);
  buttonData[1] = digitalRead(alarmSw1Pin);
  buttonData[2] = digitalRead(snoozeSwPin);

  oldButtonState[0] = buttonState[0]; //save oldButtonState
  buttonState[0] = 1 - buttonData[0]; //just invert and write state for toggle switch - no debounce needed

  for (int i = 1; i < totalButtons; i++) { //momentary buttons 1 and 2
    if ((millis() - bounceTime[i] > bounceDelay)) {
      if ((oldButtonData[i] == HIGH) && (buttonData[i] == LOW)) { //detect button press
        buttonState[i] = 1;
        holdTime[i] = millis(); //time the button started to be held

      } else if ((oldButtonData[i] == LOW) && (buttonData[i] == HIGH)) { //detect button release
        if (oldButtonState[i] == 1) buttonState[i] = 2; //release doesn't count for hold
        if (changedMode()) changedModeFlag = false; //ensure button released after changing mode, to prevent instant incrementing
      }
      oldButtonData[i] = buttonData[i]; //refresh
      bounceTime[i] = millis(); //store time for debounce
      //no debounce required for held buttons

    } else if ((millis() > holdTime[i] + longHoldDelay) && (oldButtonData[i] == LOW) && (buttonData[i] == LOW)) { //detect long button hold
      buttonState[i] = 4;
    } else if ((millis() > holdTime[i] + holdDelay) && (oldButtonData[i] == LOW) && (buttonData[i] == LOW)) { //detect button hold
      buttonState[i] = 3;
    } else buttonState[i] = 0;

    if (buttonState[i] != 0 ) oldButtonState[i] = buttonState[i]; //store previous button state
    if (oldButtonState[i] == 1 && buttonState[i] >= 3) incTime = millis(); //if button just started to be held
  }
}

void printTime() {
  ledDisplayByte[2] = dots(false, false, true); //no decimal points, dots on, flashing
  ledDisplayByte[1] = clockData[2]; //hours
  ledDisplayByte[0] = clockData[1]; //mins
  if (bcdToDec(clockData[2]) / 10 == 0) ledDecode = 4; //if leading zero, turn bank 4 off
  else ledDecode = 0;
}

//12h formatting
//  Serial.print(bcdToDec(hours & B00011111) ); //12h format mask
//  Serial.println(ampm( (hours & B00100000) >> 5)); //am/pm indicator, requires function below - 12h format only
//String ampm (byte value) {
//  if (value == 1) return "PM";
//  else return "AM";
//}

void printSeconds() {
  ledDecode = 34; //special decode on banks 3 & 4 to allow blanking
  ledDisplayByte[2] = dots(false, false, false); //no decimal points, dots off
  ledDisplayByte[1] = 0; //blank the left hand digits
  ledDisplayByte[0] = clockData[0]; //seconds
}

//display time left to snooze
void printSnoozeRemain() {
  snoozeLeft = bcdToDec((alarm1Data[1] & B01111111)) - bcdToDec(clockData[1]); //mins
  if (snoozeLeft < 0) snoozeLeft += 60; //rollover if necessary
  ledDisplayByte[2] = dots(false, true, false); //no decimal points, dots on
  ledDisplayByte[1] = 0;
  ledDisplayByte[0] = decToBcd(snoozeLeft);
  if (snoozeLeft / 10 == 0) ledDecode = 234; //if leading zero, turn bank 2, 3 & 4 off
  else ledDecode = 34; //turn off banks 3 & 4
}

//set time to snooze for
void printSnooze(boolean set) {
  ledDisplayByte[2] = dots(true, false, false); //decimal points on, dots off
  ledDisplayByte[1] = B01010110; //blank the left hand digits
  if (set) ledDisplayByte[0] = decToBcd(setSnooze); //if setting
  else ledDisplayByte[0] = decToBcd(defaultSnooze);
  if (bcdToDec(ledDisplayByte[0]) / 10 == 0) ledDecode = 923; //special decode on banks 2 & 3 to allow n and blanking
  else ledDecode = 93; //special on 3

  if ((millis() - lastSet > 1000 && even() && set) || (confirmFlash && dEven())) { //if setting and buttons not pressed and none have been pressed for over 1 second, or confirming, begin flashing
    ledDisplayByte[0] = 0; //set mins digit to 0
    ledDecode = 9123; //special on banks 9, 2 & 3
  }
}

//print alarm from RTC ***only required for testing now
void printAlarm2() {
  ledDisplayByte[2] = dots(false, true, false); //no decimal points, dots on
  ledDisplayByte[1] = alarm2Data[1] & B00111111; //hours
  ledDisplayByte[0] = alarm2Data[0] & B01111111; //mins
  if (bcdToDec(alarm2Data[1]) / 10 == 0) ledDecode = 4; //if leading zero, turn bank 4 off
  else ledDecode = 0;
}

//print alarm from EEPROM
void printAlarm(int al) {
  ledDisplayByte[2] = dots(false, true, false); //no decimal points, dots on
  ledDisplayByte[1] = decToBcd(alarmTime[al][1]); //hours
  ledDisplayByte[0] = decToBcd(alarmTime[al][0]); //mins
  if (alarmTime[al][1] / 10 == 0) ledDecode = 4; //if leading zero, turn bank 4 off
  else ledDecode = 0;
}

//general settings mode for clock and alarm
void printSetting() {
  ledDisplayByte[2] = dots(false, true, false); //no decimal points, dots on
  ledDisplayByte[1] = decToBcd(currentSetting[1]); //hours
  ledDisplayByte[0] = decToBcd(currentSetting[0]); //mins
  //  if (currentSetting[1] / 10 == 0) ledDecode = 4; //if leading zero, turn bank 4 off
  //  else ledDecode = 0;
  ledDecode = (currentSetting[1] / 10 == 0) ? 4 : 0;

  if (millis() - lastSet > 1000 && even()) { //if buttons not pressed and none have been pressed for over 1 second, begin flashing
    switch (setMode) {
      case 1:
        ledDisplayByte[1] = 0; //set hrs digit to 0
        ledDecode = 34; //turn off banks 3 & 4
        break;
      case 0:
        ledDisplayByte[0] = 0; //set mins digit to 0
        if (currentSetting[1] / 10 == 0) ledDecode = 124; //turn off banks 1, 2 & 4
        else ledDecode = 12; //turn off banks 1 & 2
        break;
    }
  }
}

// add check for negative flag and display it in place of the bank 4 digit? interference with bank 5 no decode?
void printTemp() {
  ledDisplayByte[2] = dots(true, false, false); //decimal point on, dots off
  ledDisplayByte[1] = decToBcd(int(currentTemp)); //left hand digits
  ledDisplayByte[0] = (int(currentTemp * 10) % 10) << 4 | B1100; //right hand digit with upper case C character (12)
  //  ledDisplayByte[0] = (int(currentTemp*10)%10) << 4 | B0001; //right hand digit with lower case C character - requires special decode
  if (int(currentTemp) / 10 == 0) ledDecode = 4; //if leading zero in left hand digit, turn off bank 4
  else ledDecode = 0;
}

//transition between display modes
void printTransition() {
  int del = 70; //!! testing only !!!
  byte oldT[3];
  byte newT[3];
  for (int d = 0; d < 3; d++) {
    oldT[d] = ledDisplayByte[d];
  }

  printAlarm(displayedAlarm); //update ledDisplay byte with new values

  for (int e = 0; e < 3; e++) {
    newT[e] = ledDisplayByte[e];
  }

  ledDisplayByte[2] = dots(false, false, false); //keep dots off
  ledDisplayByte[1] = oldT[1] >> 4;
  ledDisplayByte[0] = (oldT[0] >> 4) | (oldT[1] << 4);
  ledDecode = 4;
  updateDisplay();
  delay(del);

  ledDisplayByte[1] = 0;
  ledDisplayByte[0] = oldT[1];
  ledDecode = 34;
  updateDisplay();
  delay(del);

  ledDisplayByte[0] = oldT[1] >> 4;
  ledDecode = 234;
  updateDisplay();
  delay(del);

  ledDisplayByte[1] = newT[0] << 4;
  ledDisplayByte[0] = 0;
  ledDecode = 123;
  updateDisplay();
  delay(del);

  ledDisplayByte[1] = newT[0];
  ledDecode = 12;
  updateDisplay();
  delay(del);

  ledDisplayByte[1] = newT[1] << 4 | (newT[0] >> 4);
  ledDisplayByte[0] = newT[0] << 4;
  ledDecode = 1;
  updateDisplay();
  delay(del);

  mode = 5;

}

//write to the display
void updateDisplay() {
  int ledControlByte;
  ledControlByte = B11100001; //low power mode off, special decode selected for all banks but enabled on bank 5 only
  switch (ledDecode) { //decode extension modes
    case 1:
      ledControlByte |= B00000010; //special decode on bank 1
      break;
    case 2:
      ledControlByte |= B00000100; //special decode on bank 2
      break;
    case 4:
      ledControlByte |= B00010000; //special decode on bank 4
      break;
    case 12:
      ledControlByte |= B00000110; //special decode on bank 1 & 2
      break;
    case 24:
      ledControlByte |= B00010100; //special decode on banks 2 & 4
      break;
    case 34:
      ledControlByte |= B00011000; //special decode on banks 3 & 4
      break;
    case 123:
      ledControlByte |= B00001110; //special decode on banks 1, 2 & 3
      break;
    case 124:
      ledControlByte |= B00010110; //special decode on banks 1, 2 & 4
      break;
    case 234:
      ledControlByte |= B00011100; //special decode on banks 2, 3 & 4
      break;
    case 1234:
      ledControlByte |= B00011110; //special decode on banks 1, 2, 3 & 4
      break;
    case 93:
      ledControlByte |= B01001000; //special decode on bank 3
      break;
    case 923:
      ledControlByte |= B01001100; //special decode on banks 2 & 3
      break;
    case 9123:
      ledControlByte |= B01001110; //special decode on banks 2, 3 & 4
      break;
  } //else no additional decode
  oldLedByte[3] = ledDecode; //store new control byte decode

  //write data to configuration register
  digitalWrite(ledEnable, LOW);
  shiftOut(ledData, ledClock, MSBFIRST, ledControlByte);
  digitalWrite(ledEnable, HIGH);

  //write to display register
  digitalWrite(ledEnable, LOW);
  for (int d = 2; d >= 0; d--) {
    shiftOut(ledData, ledClock, MSBFIRST, ledDisplayByte[d]); // shift out brightness, decimal points and other leds data
    oldLedByte[d] = ledDisplayByte[d]; //store existing value
  }
  digitalWrite(ledEnable, HIGH);
}

void updateLeds() {
  //fade leds
  if (fadeDir) fadeValue--; //going down
  else fadeValue++; //going up
  if (fadeValue == 255 || fadeValue == 0) fadeDir = !fadeDir; //change direction

  if (alarmTriggered) {
    analogWrite (snoozeLedPin, fadeValue); //fade snooze LED
    if (dEven()) alarmLed = true; //flash alarm led
    else alarmLed = false;

  } else { //if alarm not triggered
    if (alarmSet[0]) analogWrite (snoozeLedPin, fadeValue); //if snoozing - fade snooze led
    else analogWrite (snoozeLedPin, 0);

    if (mode == 6 && setTarget == 1) { //if setting alarm
      if (even()) alarmLed = false; //flash alarm led slowly
      else alarmLed = true;
    } else if (confirmFlash && (millis() - confirmFlashTime < displayFor)) {  //if confirming setting
      switch (setTarget) {
        case 0: //if time was set
          if (dEven()) flashDots = true; //flash dots
          else flashDots = false;
          break;
        case 1: //if alarm was set
          flashDots = true; //dots should be on whilst flashing alarm confirm
          if (dEven()) alarmLed = true; //flash alarm led
          else alarmLed = false;
          break;
      }
    } else if (alarmSet[1]) alarmLed = true; //if alarm turned on, activate alarm led
    else alarmLed = false;
    if (mode == 5 & displayedAlarm != currentAlarm) alarmLed = false; //if showing an alarm, but it's not currently set
  }
}

//reset the fading of the snooze LED
void resetFade() {
  fadeValue = 0;
  fadeDir = 0;
}

//control flashing of LEDs to confirm settings
void confirm(boolean activate) {
  if (activate) {
    confirmFlash = true;
    confirmFlashTime = millis();
  } else confirmFlash = false;
}

//control of bank 5 and decimal points
byte dots(boolean decimal, boolean separator, boolean flash) { //[1XX = decimal point on, X1X = dots on, X01 = flashing dots]
  int d = 0; //counter for which dots to use **better and more clever way to do this with different pins and mathmatics?
  byte b = B10000000; //full brightness
  if (decimal) b |= B00110000; //bank 3 decimal point
  if (alarmLed) d += 1; //alarm led
  if ((confirmFlash && flashDots) || (!confirmFlash && (separator || (flash && even())))) d += 2 ; //if confirming with dots, or flashing dots or dots on
  switch (d) {
    case 1:
      b |= B00001000;
      break;
    case 2:
      b |= B00000011;
      break;
    case 3:
      b |= B00000010;
      break;
  }
  return b;
}

//control alarm sounds
void alarmSounds(int type) {
  if (type == 0) { //alarm off
    if (toneActive) {
      noTone(piezoPin);
      pinMode(piezoPin, INPUT); //stop that annoying quiet buzzing sound...
      toneReps = 0; //reset
      toneActive = false;
    }
  } else if ((millis() - toneStarted) > (toneDuration + toneDelay)) {
    toneActive = true;
    switch (type) {
      case 1:
        switch (toneReps) { //cool but harsh - some variation due to the multipliers
          case 0:
            t = 50;
            toneReps++;
            break;
          case 1:
            if (t < 2500) {
              sound(t, 60, 0);
              t *= 1.45;
            } else toneReps++;
            break;
          case 2:
            t = 50;
            //        toneDelay = 100;
            toneReps++;
            break;
          case 3:
            if (t < 4000) {
              sound(t, 20, 0);
              t *= 1.5;
            } else toneReps = 0;
            break;
        }
        break;
      case 2:
        switch (toneReps) { //siren
          case 0:
            t = 5;
            toneReps++;
            break;
          case 1:
            if (t < 1500) {
              sound(t, 10, 0);
              t += 5;
            } else toneReps++;
            break;
          case 2:
            if (t > 5) {
              sound(t, 10, 0);
              t -= 5;
            } else toneReps = 0;
            break;
        }
        break;
      case 3:
        switch (toneReps) {
          case 0:
            sound(100, 50, 120);
            toneReps++;
            break;
          case 1:
            sound(200, 50, 120);
            toneReps++;
            break;
          case 2:
            sound(300, 50, 120);
            toneReps++;
            break;
          case 3:
            sound(800, 50, 120);
            toneReps++;
            break;
          case 4:
            sound(2000, 100, 120);
            toneReps = 0;
            break;
        }
        break;
    }
  }
}

//generate sound using tone
void sound(int freq, int dur, int del) {
  tone(piezoPin, freq, dur);
  toneDuration = dur;
  toneDelay = del;
  toneStarted = millis();
}


