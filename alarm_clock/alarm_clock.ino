/* CLOCK v4
   Russ Winch
   Started February 2016

   TODO:

   Hardware
    breadboard arduino
    usb power

   Software
    improve alarm tones
    add negative for temperature
    add extra alarms
    settings menu for alarm tone and snooze time and 12/24h mode?
    write settings to ATMEGA EEPROM
    automatically exit settings menu if no buttons pressed for a while

   Bugs
    none found right now, woo!
*/


#define DS3231 0x68 //I2C address of DS3231
#include "Wire.h" //for I2C communication

//input pins
const int alarmSw0Pin = A0; //constant alarm switch
const int alarmSw1Pin = A1; //momentary alarm switch
const int snoozeSwPin = 4; //snooze switch

//output pins
//A4 & A5 reserved for I2C Clock and Data, respectively
const int piezoPin = 3; //piezo sounder
const int snoozeLedPin = 5; //snooze led
const int ledEnable = 6; //led driver enable
const int ledClock = 7; //led driver clock
const int ledData = 8; //led driver data

//menu variables
int menu = 0; //main menu selector
int oldMenu; //previous menu
boolean changedMenu = false; //flag to detect if menu has changed
int setMenu = 1; //settings menu selector
byte currentSetting[2]; //holds current variables when settings entered
byte currentSettingMax[2] = {59, 23}; //max mins and hours allowed
unsigned long lastSet; //time the last button was pressed to adjust a setting
int setTarget; //target for settings [0=clock, 1=alarm]
int displayFor = 3000; //time to display alarm before returning to clock

//clock variables
byte clockData[3]; //seconds, minutes, hours
byte alarm1Data[3]; //seconds, minutes, hours
byte alarm2Data[2]; //minutes, hours
byte tempData[2]; //integer, decimal
byte statusData; //status register
boolean even; //seconds are even - used to flash various items

//temperature variables
float temperature[3]; //the current temperature [0=sign, 1=integer, 2=decimal]
float currentTemp;
const int tempLogReadings = 10; //number of readings to average
float tempLog[tempLogReadings]; //readings for average
int tempLogIndex = 0; //index for average
float tempLogTotal = 0; //total for average
int oldSeconds; //to check if the clock has incremented

//led driver variables
byte ledDisplayByte[3]; //led display data to write back [0=D0-D7, 1=D8-D15, 2=D16-D23]
byte oldLedByte[4]; //data from the last update
int ledDecode = 0; //decode mode - used to turn off banks

//led variables
int fadeValue = 0;
boolean fadeDir = false;
boolean alarmLed = false;
boolean flashDots = false;
boolean confirmFlash = false;
unsigned long confirmFlashTime; //time led flashing started when confirming setting

//button variables
const int totalButtons = 3; //total connected buttons to check
boolean buttonData[totalButtons]; //current button reading
boolean oldButtonData[totalButtons] = {LOW, HIGH, HIGH}; //previous button reading
int buttonState[totalButtons]; //current button state [0=alSw0, 1= alSw1, 2=snSw]
int oldButtonState[totalButtons]; //previous button state
unsigned long bounceTime[totalButtons]; //millis() times for each button last pressed for debounce
unsigned long holdTime[totalButtons];
unsigned long incTime; //time since last increment
int bounceDelay = 40; //debounce time
int holdDelay = 1000; //time to wait for button hold
int longHoldDelay = 2000; //time to wait for long button hold
int incDelay = 80; //time to wait between incrementing during button hold - speed

//alarm variables
const int totalAlarms = 2;
boolean alarmStatus[totalAlarms]; //holds the alarm triggered flags [0=AL1, 1=AL2]
boolean alarmSet[totalAlarms]; //holds the alarm set flags [0=AL1, 1=AL2]
boolean alarmTriggered = false;
int snoozeFor = 5; //snooze for 5  mins - ***turn into a setting

//alarm tones
boolean toneActive = false; //is there an active tone
unsigned long toneStarted = 0; ///millis when tone started
int toneDuration = 0; //tone duration
int toneDelay = 0; //delay between tones
int toneReps = 0; //cycle through the reps for the sound
int t; //tone frequency variable


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

  //debugging stuff
  //  Serial.begin(9600);
  //  resetAlarms();
  //  setTime(0,3,10); //Hours,Mins,Secs
  //  setAlarm1(8,30);
  //  setAlarm2(7,30);
}

void loop() {
  //check for menu change
  if (oldMenu != menu) changedMenu = true;
  oldMenu = menu; //store previous mode

  getButtons();

  //check buttons whilst in display mode
  if (menu < 5) {
    if (buttonState[1] == 1) { //if al2 switch released
      if (alarmSet[0]) menu = 4; //if snoozing
      else menu = 3; //show alarm
      lastSet = millis();
    }
    if (buttonState[2] == 2) { //if snooze switch released
      menu++; //next display mode
      if (menu > 2) menu = 0; //loop round
    }
  }

  if (menu == 5 && !changedMenu) { //if in settings menu and not still holding button from entering settings menu
    if (buttonState[2] == 1 || ((buttonState[2] >= 3) && (millis() - incTime > incDelay))) { //if snooze switch pressed or held or longheld and delay since last increment
      currentSetting[setMenu]++; //increment current setting
      if (currentSetting[setMenu] > currentSettingMax[setMenu]) currentSetting[setMenu] = 0; //wrap current setting
      lastSet = millis(); //set time since last set - so they start flashing after delay
      incTime = millis(); //set time since last increment - to prevent settings changing too quickly
    } else if (buttonState[1] == 1) { //if al2 switch pressed. else used incase both buttons held together
      setMenu--; //goto next setting
      if (setMenu < 0) { //if past end of settings - save and exit
        setMenu = 1; //reset menu pointer
        confirmFlash = true;
        confirmFlashTime = millis();
        switch (setTarget) { //determines target for settings
          case 0: //clock
            setTime(currentSetting[1], currentSetting[0], 0);
            menu = 0;
            break;
          case 1: //alarm2
            setAlarm2(currentSetting[1], currentSetting[0]);
            menu = 3;
            lastSet = millis(); //save time of setting so alarm can be displayed for a short while
            break;
        }
      }
    }
  }
  
  //when confirm flash has finished - reset
  if (millis() - confirmFlashTime > displayFor) confirmFlash = false; 

  //if snooze switch long held when showing clock
  if (menu == 0 && buttonState[2] == 4 && !alarmTriggered) {
    currentSetting[0] = bcdToDec(clockData[1]); //mins
    currentSetting[1] = bcdToDec(clockData[2]); //hrs
    setTarget = 0; //set clock
    menu = 5; //settings
  }

  //if alarm switch long held when showing alarm
  if (menu == 3 && buttonState[1] == 4) {
    currentSetting[0] = bcdToDec(alarm2Data[0]); //mins
    currentSetting[1] = bcdToDec(alarm2Data[1]); //hrs
    setTarget = 1; //set alarm
    menu = 5; //settings
  }

  //if alarm displayed without button press, return to clock
  if ((menu == 3 || menu == 4) && (millis() - lastSet > displayFor)) menu = 0;


  //if setting displayed without button press for 30 seconds, return to clock
  //  if ((menu == 5 && (millis() - lastSet > 30000)) menu = 0;
  //** not working - lastSet is not set when entering settings menu - if it was, flashing would be delayed and it would not be obvious settings had been entered

  //check alarms
  if (((alarmStatus[0] && alarmSet[0]) || (alarmStatus[1] && alarmSet[1])) && !alarmTriggered) {
    alarmTriggered = true;
    resetFade(); // start fading leds from 0
    menu = 0; //return to clock
  }

  //if exiting settings do not trigger alarm - needed incase time is set to alarm time
  if (oldMenu == 5 && menu == 0) resetAlarms();

  //if alarm currently triggered
  if (alarmTriggered) {
    alarmSounds(3);
    if (buttonState[1] == 1) { //if alarm momentary switch pressed when alarm triggered - reset
      resetAlarms();
      menu = 0; //show time
    }
    if (buttonState[2] == 2) {
      snooze(); //if snooze switch released - snooze
      menu = 0; //show time
    }
  } else alarmSounds(0);

  //if alarm switch toggled off and currently in settings - exit setting
  if (buttonState[0] == 1 && oldButtonState[0] == 0 && menu >= 4) menu = 0;

  //alarm switch
  if (buttonState[0] == 0) { //if al1 switch on
    if (!alarmSet[1] && alarmStatus[1]) resetAlarms(); //if just switched on and alarm triggered
    alarmSet[1] = true; //turn on alarm
    //    alarmSounds(1); //testing
  } else { //if alarm switch off
    alarmSet[1] = false; //deactivate alarms
    if (alarmTriggered || alarmSet[0]) resetAlarms(); //if alarm triggered or snoozing
    //    alarmSounds(0); //testing
  }

  getTime();
  getTemp();
  getAlarm1();
  getAlarm2();
  getAlarmStatus();
  updateLeds(); //integrate into update display?

  //main menu
  switch (menu) {
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
      printAlarm2();
      break;
    case 4:
      printAlarm1();
      break;
    case 5:
      printSetting();
      break;
  }

  //check for changes to display since last refresh  (warning in datasheet about potential dim segments with continuous writing of same data)
  int a = 0;
  for (int c = 0; c < 3; c++) { //check for changes since last refresh
    if (oldLedByte[c] != ledDisplayByte[c]) a++; //if any display bytes have changed
  }
  if (oldLedByte[3] != ledDecode) a++; //if any changes required to control byte
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

void snooze() {
  resetAlarms();
  int snoozeAlarm[3] = { bcdToDec(clockData[0]), bcdToDec(clockData[1]) + snoozeFor, bcdToDec(clockData[2]) }; //secs,mins,hrs
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

//one call to fill all arrays!?
//void getDS3231() {
//  Wire.beginTransmission(DS3231);
//  Wire.write(0x00); //read from byte 0 (seconds)
//  Wire.requestFrom(DS3231, 18); //request 18 bytes - seconds, minutes, hours...
//  for (int i=0; i<3; i++) {
//    clockData[i] = Wire.read();
//  }
//  int w;
//  while (w < 4){
//    Wire.read();//4: read 4 blank bytes
//    w++;
//  }
//  for (int i=0; i<3; i++) { //7
//    alarm1Data[i] = Wire.read();
//  }
//  Wire.read();//10: read 1 blank byte
//  for (int i=0; i<2; i++) {//11
//    alarm2Data[i] = Wire.read();
//  }
//  Wire.read();//13: read 2 blank bytes
//  Wire.read();
//  statusData = Wire.read();//15
//  Wire.read(); //16: read 1 blank byte
//  tempData[0] = Wire.read(); //17  sign, integer
//  tempData[1] = Wire.read() >> 6; //decimal, shift right as only bytes 7 and 8 are used, +-0.25 accuracy
//}

void getTime() {
  Wire.beginTransmission(DS3231);
  Wire.write(0x00); //read from byte 0 (seconds)
  Wire.endTransmission();
  Wire.requestFrom(DS3231, 3); //request 3 bytes - seconds, minutes, hours
  for (int i = 0; i < 3; i++) {
    clockData[i] = Wire.read();
  }
  if (clockData[0] % 2) even = false; //check if seconds is even - for flashing
  else even = true;
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
    if (tempLogIndex >= tempLogReadings) //loop array pointer
      tempLogIndex = 0;
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

  for (int i = 1; i <= 2; i++) { //momentary buttons 1 and 2
    oldButtonState[i] = buttonState[i]; //store previous button state
    if (millis() - bounceTime[i] > bounceDelay) {
      if ((oldButtonData[i] == HIGH) && (buttonData[i] == LOW)) { //detect button press
        buttonState[i] = 1;
        holdTime[i] = millis(); //time the button started to be held
      } else if ((oldButtonData[i] == LOW) && (buttonData[i] == HIGH)) { //detect button release
        buttonState[i] = 2;
        if (changedMenu) changedMenu = false; //ensure button released after changing menu, to prevent instant incrementing
      }
      oldButtonData[i] = buttonData[i]; //refresh
      bounceTime[i] = millis(); //store time for debounce
      //no debounce required for held buttons
    } else if ((millis() > holdTime[i] + longHoldDelay) && (oldButtonData[i] == LOW) && (buttonData[i] == LOW)) { //detect long button hold
      buttonState[i] = 4;
    } else if ((millis() > holdTime[i] + holdDelay) && (oldButtonData[i] == LOW) && (buttonData[i] == LOW)) { //detect button hold
      buttonState[i] = 3;
    } else buttonState[i] = 0;
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
void printAlarm1() {
  ledDecode = 34; //turn off banks 3 & 4
  int snoozeLeft = bcdToDec((alarm1Data[1] & B01111111)) - bcdToDec(clockData[1]); //mins
  if (snoozeLeft < 0) snoozeLeft += 60; //rollover
  ledDisplayByte[2] = dots(false, true, false); //no decimal points, dots on
  ledDisplayByte[1] = 0;
  ledDisplayByte[0] = decToBcd(snoozeLeft);
  //  if (millis() - lastSet > 3000) menu = 0; //if displayed for more than 3 seconds without button press, return to clock
}

//*** add additional alarms here? or in the settings?
void printAlarm2() {
  ledDisplayByte[2] = dots(false, true, false); //no decimal points, dots on
  ledDisplayByte[1] = alarm2Data[1] & B00111111; //hours
  ledDisplayByte[0] = alarm2Data[0] & B01111111; //mins
  if (bcdToDec(alarm2Data[1]) / 10 == 0) ledDecode = 4; //if leading zero, turn bank 4 off
  else ledDecode = 0;
}

//general settings menu for both clock and all alarms
void printSetting() {
  ledDisplayByte[2] = dots(false, true, false); //no decimal points, dots on
  ledDisplayByte[1] = decToBcd(currentSetting[1]); //hours
  ledDisplayByte[0] = decToBcd(currentSetting[0]); //mins
  if (currentSetting[1] / 10 == 0) ledDecode = 4; //if leading zero, turn bank 4 off
  else ledDecode = 0;

  if (millis() - lastSet > 1000 && even) { //if buttons not pressed and none have been pressed for over 1 second, begin flashing
    switch (setMenu) {
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
  ledDisplayByte[0] = (int(currentTemp * 10) % 10) << 4 | B1100; //right hand digit with upper case C character
  //  ledDisplayByte[0] = (int(currentTemp*10)%10) << 4 | B0001; //right hand digit with lower case C character - requires special decode
  if (int(currentTemp) / 10 == 0) ledDecode = 4; //if leading zero in left hand digit, turn off bank 4
  else ledDecode = 0;
}

//write to the display
void updateDisplay() {
  int ledControlByte;
  ledControlByte = B00100001; //low power mode off, no decode on bank 5 only
  switch (ledDecode) { //no decode extension modes
    case 2:
      ledControlByte |= B00000100; //no decode on bank 2
      break;
    case 4:
      ledControlByte |= B00010000; //no decode on bank 4
      break;
    case 12:
      ledControlByte |= B00000110; //no decode on bank 1 & 2
      break;
    case 24:
      ledControlByte |= B00010100; //no decode on banks 2 & 4
      break;
    case 34:
      ledControlByte |= B00011000; //no decode on banks 3 & 4
      break;
    case 124:
      ledControlByte |= B00010110; //no decode on banks 1, 2 & 4
      break;
  } //else no additional decode
  oldLedByte[3] = ledDecode; //store new control byte decode

  //enable low power mode - fixes glitchiness when switching between decode modes
  digitalWrite(ledEnable, LOW); //set enable pin to low to enable writing
  shiftOut(ledData, ledClock, MSBFIRST, 0);
  digitalWrite(ledEnable, HIGH); //update with new data

  //write to display register
  digitalWrite(ledEnable, LOW);
  for (int d = 2; d >= 0; d--) {
    shiftOut(ledData, ledClock, MSBFIRST, ledDisplayByte[d]); // shift out brightness, decimal points and other leds data
    oldLedByte[d] = ledDisplayByte[d];
  }
  digitalWrite(ledEnable, HIGH);

  //disable low power mode and write data to configuration register
  digitalWrite(ledEnable, LOW);
  shiftOut(ledData, ledClock, MSBFIRST, ledControlByte);
  digitalWrite(ledEnable, HIGH);
}

void updateLeds() {
  //fade leds
  if (fadeDir) fadeValue--; //going down
  else fadeValue++; //going up
  if (fadeValue == 255 || fadeValue == 0) fadeDir = 1 - fadeDir; //change direction

  if (alarmTriggered) {
    analogWrite (snoozeLedPin, fadeValue);
    if ((millis() / 100) % 2) alarmLed = true; //flash alarm led
    else alarmLed = false;

  } else { //if alarm not triggered
    if (alarmSet[0]) analogWrite (snoozeLedPin, fadeValue); //if snoozing - fade snooze led
    else analogWrite (snoozeLedPin, 0);

    //if setting alarm
    if (menu == 5 && setTarget == 1) {
      if (even) alarmLed = false; //flash alarm led slowly
      else alarmLed = true;
    //if confirming setting
    } else if (confirmFlash && (millis() - confirmFlashTime < displayFor)) { 
      if (setTarget == 0) { //if time was set
        if ((millis() / 100) % 2) flashDots = true; //flash dots
        else flashDots = false;
      }
      if (setTarget == 1) { //if alarm was set
        if ((millis() / 100) % 2) alarmLed = true; //flash alarm led
        else alarmLed = false;
      }      
    } else if (alarmSet[1]) alarmLed = true; //if alarm turned on, activate alarm led
    else alarmLed = false;
  }
}

void resetFade() {
  fadeValue = 0;
  fadeDir = 0;
}

//control of bank 5 and decimal points
byte dots(boolean decimal, boolean separator, boolean flash) { //[1XX = decimal point on, X1X = dots on, X01 = flashing dots]
  byte b = B10000000; //full brightness
  if (decimal) b |= B00110000; //bank 3 decimal point
  if (alarmLed) b |= B00000010; //alarm led
  if ((confirmFlash && flashDots) || (!confirmFlash && (separator || (flash && even)))) b |= B00000100; //separator dots
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
            break;
        }

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


