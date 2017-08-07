#include <AssetTrackerRK.h>
#include <HttpClient.h>

#define MINIMUM_PERIOD      20000       //  20 seconds minimum between publish attempts
#define RUNNING_HEART       120000      //  2 minutes maximum between pushings (when powered)
#define MAXIMUM_PERIOD      3600000     //  1 hour between publishings (when unpowered)
#define CELLULAR_WATCHDOG   600000      // 10 minutes to restart device if no cell connection can be made
#define MOVING_THRESHOLD    10          // 10 meters between positions consitutes movement
#define SLEEP_TRIGGER       300000      //  5 minutes to go to sleep
#define STAY_AWAKE_TIMEOUT  6 * 60 * 60 * 1000 // 6 hours

void refreshGPSAlt();

int tmp102Address           = 0x48;
float tempF                 = 0;
float soc                   = 0;
int rssi                    = 0;

bool gettingFix             = false;
bool waitingToSleep         = false;
bool sleepingHeartbeat      = false;
bool sleepPublish           = false;
bool publishSuccess         = false;
bool stayAwake              = false;

float curLat                = 0;
float curLng                = 0;
float prvLat                = 0;
float prvLng                = 0;
float course                = 0;
float speed                 = 0;

long lastPublish            = 0;
long lastPower              = 0;
long startFix               = 0;
long cellularWatch          = 0;
long stayAwakeTimeout       = 0;

String iccid                = "";

retained int publishSinceLastPower   = 0;

TinyGPSPlus gps;
FuelGauge fuel;
CellularSignal sig;

STARTUP(System.enableFeature(FEATURE_RESET_INFO));
//SYSTEM_THREAD(ENABLED);

/*
*  0 = RESTART
*  1 = MOVING
*  2 = HEARTBEAT
*  3 = MANUAL REQUEST
*  4 = POWER REMOVED
*  5 = POWER APPLIED
*/

void setup() {

    Particle.disconnect();

    Wire.begin();

    // GPS SETUP
    Serial1.begin(9600);
    pinMode(D6, OUTPUT);
    digitalWrite(D6, LOW);
    startFix = cellularWatch = millis();
    gettingFix = true;

    // GET ICCID
    CellularDevice device;
    memset(&device, 0, sizeof(device));
    device.size = sizeof(device);
    cellular_device_info(&device, NULL);
    iccid = device.iccid;

    if (System.resetReason() == RESET_REASON_POWER_MANAGEMENT){
        if (pwrPresent()){
            publishSinceLastPower = 0;
            publishGPS(5);
        }
        else{
            sleepingHeartbeat = true;
            sleepPublish = false;
            lastPower = millis();
        }
    }
    else{
        // Publish Restart
        publishGPS(0);
    }
}

void loop() 
{
    
    // If we're not supposed to awake, reset the timer and disconnect from Particle
    if (!stayAwake)
    {
        stayAwakeTimeout = millis();
        if (Particle.connected())
        {
            Particle.disconnect();
        }
    }
    // If we're supposed to be awake, make sure we're connected to Particle
    else
    {
         if (!Particle.connected())
         {
            Particle.connect();
         }   
    }
    
    // It's time to stop staying awake
    if (millis() - stayAwakeTimeout > STAY_AWAKE_TIMEOUT)
    {
        stayAwake = false;
    }

    
    if (!Cellular.ready())
    {
        Cellular.connect();
        if (millis() - cellularWatch > CELLULAR_WATCHDOG)
        {
            System.reset();
        }
    }
    else
    {
        cellularWatch = millis();    
    }
    
    // We'll allow a while here to clear the serial buffer
    while (Serial1.available() > 0) 
    {
        if (gps.encode(Serial1.read())) 
        {
            refreshGPSAlt();
        }
    }
  
    if (!gettingFix && !sleepingHeartbeat)
    { 
        // If we're moving and met our minimum delay, do a publish
        if ((millis() - lastPublish) > MINIMUM_PERIOD && isMoving())
        { 
                // Publish Moving
                publishGPS(1);
        }
        
        // If we've met out maximum delay, do a publish
        if (millis() - lastPublish > RUNNING_HEART)
        {
            // Publish Heartbeat
            publishGPS(2);
        }
    
    }
    
    // If power is present, reset the lastPower counter, and reset our publish back off counter
    if (pwrPresent())
    {
        lastPower = millis();
        waitingToSleep = false;
        sleepingHeartbeat = false;
        publishSinceLastPower = 0;
    }
    else
    {
        if (!waitingToSleep) // Power is removed but we were not previously asleep -- as in, power was probably just removed
        {
            if (!gettingFix){
                waitingToSleep = true; // Keep track of the fact that we previously acted on this power removal
                if (sleepingHeartbeat){
                    publishGPS(2); // publish a POWER REMOVED position report
                }
                else{
                    publishGPS(4); // publish a POWER REMOVED position report
                }
                sleepPublish = true;
            }
        }
        // We've met our sleep time out OR we've already published for this wake period AND we have successfully published... OR we haven't had a GPS fix in over 20 minutes
        if (((((millis() - lastPower) > SLEEP_TRIGGER) || sleepPublish)) || (gettingFix && (millis() - startFix > 1200000)))
        { 
            if (publishSinceLastPower > 12)
            {
                publishSinceLastPower = 12;
            }
 
            // Sleep!!  Increasing the duration of sleep based on number previous publishings
            if (!stayAwake)
            {
                int sleepTime = (((MAXIMUM_PERIOD / 1000) * (publishSinceLastPower)) + 1);
                // If sleep requested is less than 5m or more than 12h, default to 1h
                if (sleepTime < 300 || sleepTime > 43200){
                    sleepTime = 3600;
                }
                System.sleep(SLEEP_MODE_DEEP, sleepTime);
            }
            else
            {
                // Particle.publish("Stay Awake: " + String(millis() - stayAwakeTimeout) + "ms / " + String(STAY_AWAKE_TIMEOUT) + "ms");
                sleepingHeartbeat = false;
                delay(((MAXIMUM_PERIOD) * (publishSinceLastPower)) + 1);
            }
        } 
    }
}

void smartdelay(unsigned long ms)
{
    unsigned long start = millis();
    do 
    {
        while (Serial1.available())
        gps.encode(Serial1.read());
    } while (millis() - start < ms);
}

void publishGPS(int tReason){
    publishSuccess = false;
    int publishAttempt = 0;
    
    while (!publishSuccess){
        HttpClient http;
        http_header_t headers[] = 
            {
                { "Content-Type", "application/x-www-form-urlencoded" },
                // { "Accept" , "*/*"},
                { NULL, NULL } // NOTE: Always terminate headers will NULL
            };
    
        http_request_t request;
        http_response_t response;

        request.hostname = "www.ethanms.com";
        request.port = 80;
        request.path = "/trackerAdd2.php";
        request.body = generateJSON(tReason);
        
        http.post(request, response, headers);

        // http.get(request, response, headers);
        
        if (response.body == "OK" || response.body == "STAY_AWAKE"){
            publishSuccess = true;
            if (!pwrPresent() && publishSinceLastPower < 12){
                publishSinceLastPower++;
            }
            prvLat = curLat;
            prvLng = curLng;
            lastPublish = millis();
            if (response.body == "STAY_AWAKE")
            {
                // Particle.publish("StayAwake");
                stayAwake = true;
                stayAwakeTimeout = millis();
            }
        }
        else{
            publishAttempt++;
            smartdelay(30000);
            if (publishAttempt > 5){
                System.reset();
            }
        }
    }

}

float getCharge(String command)
{
    refreshBattery();
    return soc;
}

void refreshBattery(){
    soc  = fuel.getSoC(); 
}

void refreshCellular(){
    sig  = Cellular.RSSI();
    rssi = sig.rssi;
}


bool pwrPresent(){
    return (digitalRead(WKP) == HIGH);
}

void refreshGPSAlt()
{
    if (gps.location.isValid()) {
        curLat = gps.location.lat();
        curLng = gps.location.lng();
        course = gps.course.deg();
        speed = gps.speed.mph();
        if (gettingFix) {
            gettingFix = false;
            unsigned long elapsed = millis() - startFix;
            startFix = millis();
        }
    }
    else {
        if (!gettingFix) {
            gettingFix = true;
            startFix = millis();
        }
    }
    delay(5);
}

float distanceCalc(float lat0, float lon0, float lat1, float lon1){
    double deltaLat = fabs(lat0 - lat1) * 111194.9;
    double deltaLon = 111194.9 * fabs(lon0 - lon1) * cos(radians((lat0 + lat1) / 2));
    return (sqrt(pow(deltaLat,2) + pow(deltaLon,2)));
}

float radians(float degrees)
{
    return (degrees * 71) / 4068;
}

bool isMoving(){
    // If power is present us the standard moving threshold, otherwise increase it by a factor of 10
    return (distanceCalc(curLat, curLng, prvLat, prvLng) > (MOVING_THRESHOLD * (pwrPresent() ? 1 : 10)));
}

String generateJSON(int tReason)
{
    refreshCellular();
    refreshBattery();
    
    String message = "{";
    message.concat("\"tracker\":");
    message.concat("\"" + iccid + "\",");

    message.concat("\"soc\":");
    message.concat("\"" + String(soc,1) + "\",");

    message.concat("\"lat\":");
    message.concat("\"" + String(curLat,6) + "\",");
    
    message.concat("\"lng\":");
    message.concat("\"" + String(curLng,6) + "\",");
    
    message.concat("\"crs\":");
    message.concat("\"" + String(course,0) + "\",");
    
    message.concat("\"spd\":");
    message.concat("\"" + String(speed,0) + "\",");    

    message.concat("\"tmp\":");
    message.concat("\"" + String(getTemperature(),0) + "\",");

    message.concat("\"rssi\":");
    message.concat("\"" + String(rssi) + "\",");
    
    message.concat("\"reason\":");
    message.concat("\"" + String(tReason) + "\"}");
    return message;
}

char* strToCharArray(String str)
{
    char tChar[str.length()];
    str.toCharArray(tChar, str.length());
    return tChar;
}

float getTemperature() {
    byte MSB=0;
    byte LSB=0;
    Wire.requestFrom(tmp102Address,2);
    
    MSB = Wire.read();
    LSB = Wire.read();
    
    int16_t TemperatureSum = ((MSB << 8) | LSB) >> 4;
    
    float fahrenheit;
    
    if(TemperatureSum & 0b100000000000)
    {
        TemperatureSum -= 1;
        TemperatureSum = ~ TemperatureSum;
        TemperatureSum = TemperatureSum & 0b111111111111;
        fahrenheit = (TemperatureSum * -0.1125) + 32;  
    }
    else
    {
        fahrenheit = (TemperatureSum * 0.1125) + 32;  
    }
    
    return fahrenheit;
}
