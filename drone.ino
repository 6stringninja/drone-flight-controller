/**
 * The software is provided "as is", without any warranty of any kind.
 * Feel free to edit it if needed.
 * 
 * @author lobodol <grobodol@gmail.com>
 */

// ---------------------------------------------------------------------------
#include <Wire.h>
// ------------------- Define some constants for convenience -----------------
#define CHANNEL1 0
#define CHANNEL2 1
#define CHANNEL3 2
#define CHANNEL4 3

#define YAW      0
#define PITCH    1
#define ROLL     2
#define THROTTLE 3

#define X           0     // X axis
#define Y           1     // Y axis
#define Z           2     // Z axis
#define MPU_ADDRESS 0x68  // I2C address of the MPU-6050
#define SSF_GYRO    65.5  // Sensitivity Scale Factor of the gyro from datasheet
// ---------------- Receiver variables ---------------------------------------
// Received instructions formatted with good units, in that order : [Yaw, Pitch, Roll, Throttle]
float instruction[4];

// Previous state of each channel (HIGH or LOW)
volatile byte previous_state[4];

// Duration of the pulse on each channel of the receiver in µs (must be within 1000µs & 2000µs)
volatile unsigned int pulse_length[4] = {1500, 1500, 1000, 1500};

// Used to calculate pulse duration on each channel.
volatile unsigned long current_time;
volatile unsigned long timer[4]; // Timer of each channel.

// Used to configure which control (yaw, pitch, roll, throttle) is on which channel.
int mode_mapping[4];
// ----------------------- MPU variables -------------------------------------
// The RAW values got from gyro (in °/sec) in that order: X, Y, Z
int gyro_raw[3] = {0,0,0};

// Average gyro offsets of each axis in that order: X, Y, Z
long gyro_offset[3] = {0, 0, 0};

// The RAW values got from accelerometer (in m/sec²) in that order: X, Y, Z
int acc_raw[3] = {0 ,0 ,0};

long acc_total_vector;

int temperature;
float angle_pitch, angle_roll;
boolean initialized; // Init flag set to TRUE after first loop
float angle_roll_acc, angle_pitch_acc;
// ----------------------- Variables for servo signal generation -------------
unsigned long loop_timer;
unsigned long now, difference;

unsigned long pulse_length_esc1 = 1000,
        pulse_length_esc2 = 1000,
        pulse_length_esc3 = 1000,
        pulse_length_esc4 = 1000;

// ------------- Global variables used for PID automation --------------------
float errors[3];                     // Measured errors (compared to instructions) : [Yaw, Pitch, Roll]
float error_sum[3]      = {0, 0, 0}; // Error sums (used for integral component) : [Yaw, Pitch, Roll]
float previous_error[3] = {0, 0, 0}; // Last errors (used for derivative component) : [Yaw, Pitch, Roll]
float measures[3]       = {0, 0, 0}; // Angular measures : [Yaw, Pitch, Roll]
// ---------------------------------------------------------------------------

/**
 * Setup configuration
 */
void setup() {
    // Turn LED on during setup.
    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH);

    setupMpu6050Registers();

    calibrateMpu6050();

    // Set pins 4 5 6 7 as outputs.
    DDRD |= B11110000;

    // Initialize loop_timer.
    loop_timer = micros();

    configureChannelMapping();

    // Start I2C communication
    Wire.begin();
    TWBR = 24; // 400kHz I2C clock (200kHz if CPU is 8MHz)

    // Configure interrupts for receiver.
    PCICR  |= (1 << PCIE0);  //Set PCIE0 to enable PCMSK0 scan.
    PCMSK0 |= (1 << PCINT0); //Set PCINT0 (digital input 8) to trigger an interrupt on state change.
    PCMSK0 |= (1 << PCINT1); //Set PCINT1 (digital input 9) to trigger an interrupt on state change.
    PCMSK0 |= (1 << PCINT2); //Set PCINT2 (digital input 10)to trigger an interrupt on state change.
    PCMSK0 |= (1 << PCINT3); //Set PCINT3 (digital input 11)to trigger an interrupt on state change.

    // Turn LED off now setup is done.
    digitalWrite(13, LOW);
}


/**
 * Main program loop
 */
void loop() {
    // 1. First, read angular values from MPU-6050
    readSensor();
    convertRawValues();

    // 2. Then, translate received data into usable values
    getFlightInstruction();

    // 3. Calculate errors comparing received instruction with measures
    calculateErrors();

    // 4. Calculate motors speed with PID controller
    automation();

    // 5. Apply motors speed
    applyMotorSpeed();
}

/**
 * Generate servo-signal on digital pins #4 #5 #6 #7 with a frequency of 250Hz (4ms period).
 * Direct port manipulation is used for performances.
 *
 * This function might not take more than 2ms to run, which lets 2ms remaining to do other stuff.
 *
 * @see https:// www.arduino.cc/en/Reference/PortManipulation
 *
 * @return void
 */
void applyMotorSpeed() {
    // Refresh rate is 250Hz: send ESC pulses every 4000µs
    while ((now = micros()) - loop_timer < 4000);

    // Update loop timer
    loop_timer = now;

    // Set pins #4 #5 #6 #7 HIGH
    PORTD |= B11110000;

    // Wait until all pins #4 #5 #6 #7 are LOW
    while (PORTD >= 16) {
        now        = micros();
        difference = now - loop_timer;

        if (difference >= pulse_length_esc1) PORTD &= B11101111; // Set pin #4 LOW
        if (difference >= pulse_length_esc2) PORTD &= B11011111; // Set pin #5 LOW
        if (difference >= pulse_length_esc3) PORTD &= B10111111; // Set pin #6 LOW
        if (difference >= pulse_length_esc4) PORTD &= B01111111; // Set pin #7 LOW
    }
}

/**
 * Request raw values from MPU6050.
 * 
 * @return void
 */
void readSensor() {
    Wire.beginTransmission(MPU_ADDRESS); // Start communicating with the MPU-6050
    Wire.write(0x3B);                    // Send the requested starting register
    Wire.endTransmission();              // End the transmission
    Wire.requestFrom(MPU_ADDRESS,14);    // Request 14 bytes from the MPU-6050

    // Wait until all the bytes are received
    while(Wire.available() < 14);

    acc_raw[X]  = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the acc_raw[X] variable
    acc_raw[Y]  = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the acc_raw[Y] variable
    acc_raw[Z]  = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the acc_raw[Z] variable
    temperature = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the temperature variable
    gyro_raw[X] = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the gyro_raw[X] variable
    gyro_raw[Y] = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the gyro_raw[Y] variable
    gyro_raw[Z] = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the gyro_raw[Z] variable
}

/**
 * Convert RAW values from gyro and accelerometer to usable values.
 * Pitch and Roll are converted in degrees
 * Yaw is convert in degree/sec (an angular rate).
 *
 * @return void
 */
void convertRawValues() {
    gyro_raw[X] -= gyro_offset[X];                                                //Subtract the offset calibration value from the raw gyro_raw[X] value
    gyro_raw[Y] -= gyro_offset[Y];                                                //Subtract the offset calibration value from the raw gyro_raw[Y] value
    gyro_raw[Z] -= gyro_offset[Z];                                                //Subtract the offset calibration value from the raw gyro_raw[Z] value

    //Gyro angle calculations
    //0.0000611 = 1 / (250Hz / 65.5)
    angle_pitch += gyro_raw[X] * 0.0000611;                                   //Calculate the traveled pitch angle and add this to the angle_pitch variable
    angle_roll += gyro_raw[Y] * 0.0000611;                                    //Calculate the traveled roll angle and add this to the angle_roll variable

    //0.000001066 = 0.0000611 * (3.142(PI) / 180degr) The Arduino sin function is in radians
    angle_pitch += angle_roll * sin(gyro_raw[Z] * 0.000001066);               //If the IMU has yawed transfer the roll angle to the pitch angel
    angle_roll -= angle_pitch * sin(gyro_raw[Z] * 0.000001066);               //If the IMU has yawed transfer the pitch angle to the roll angel

    //Accelerometer angle calculations
    acc_total_vector = sqrt((acc_x*acc_x)+(acc_y*acc_y)+(acc_z*acc_z));  //Calculate the total accelerometer vector
    //57.296 = 1 / (3.142 / 180) The Arduino asin function is in radians
    angle_pitch_acc = asin((float)acc_y/acc_total_vector)* 57.296;       //Calculate the pitch angle
    angle_roll_acc = asin((float)acc_x/acc_total_vector)* -57.296;       //Calculate the roll angle

    //Place the MPU-6050 spirit level and note the values in the following two lines for calibration
    angle_pitch_acc -= 0.0;                                              //Accelerometer calibration value for pitch
    angle_roll_acc -= 0.0;                                               //Accelerometer calibration value for roll

    if (initialized) {                                                 //If the IMU is already started
        angle_pitch = angle_pitch * 0.9996 + angle_pitch_acc * 0.0004;     //Correct the drift of the gyro pitch angle with the accelerometer pitch angle
        angle_roll = angle_roll * 0.9996 + angle_roll_acc * 0.0004;        //Correct the drift of the gyro roll angle with the accelerometer roll angle
    } else {                                                                //At first start
        angle_pitch = angle_pitch_acc;                                     //Set the gyro pitch angle equal to the accelerometer pitch angle
        angle_roll = angle_roll_acc;                                       //Set the gyro roll angle equal to the accelerometer roll angle
        initialized = true;                                            //Set the IMU started flag
    }

    //To dampen the pitch and roll angles a complementary filter is used
    measures[ROLL]  = measures[ROLL] * 0.9 + angle_pitch * 0.1;   //Take 90% of the output pitch value and add 10% of the raw pitch value
    measures[PITCH] = measures[PITCH] * 0.9 + angle_roll * 0.1;      //Take 90% of the output roll value and add 10% of the raw roll value
    measures[YAW]   = gyro_raw[Z] / SSF_GYRO;
}

/**
 * Calculate motor speed for each motor of an X quadcopter depending on received instructions and measures from sensor
 * by applying PID control.
 *
 * (A) (B)     x
 *   \ /     z ↑
 *    X       \|
 *   / \       +----→ y
 * (C) (D)
 *
 * Motors A & D run clockwise.
 * Motors B & C run counter-clockwise.
 *
 * Each motor output is considered as a servomotor. As a result, value range is about 1000µs to 2000µs
 * 
 * @return void
 */
void automation() {
    float Kp[3]       = {10, 10, 10}; // P coefficients in that order : Yaw, Pitch, Roll //ku = 0.21
    float Ki[3]       = {0.0, 0, 0};  // I coefficients in that order : Yaw, Pitch, Roll
    float Kd[3]       = {0, 0, 0};    // D coefficients in that order : Yaw, Pitch, Roll
    float deltaErr[3] = {0, 0, 0};    // Error deltas in that order :  Yaw, Pitch, Roll
    float yaw         = 0;
    float pitch       = 0;
    float roll        = 0;

    // Initialize motor commands with throttle
    pulse_length_esc1 = instruction[THROTTLE];
    pulse_length_esc2 = instruction[THROTTLE];
    pulse_length_esc3 = instruction[THROTTLE];
    pulse_length_esc4 = instruction[THROTTLE];

    // Do not calculate anything if throttle is 0
    if (instruction[THROTTLE] >= 1012) {
        // Calculate sum of errors : Integral coefficients
        error_sum[YAW] += errors[YAW];
        error_sum[PITCH] += errors[PITCH];
        error_sum[ROLL] += errors[ROLL];

        // Calculate error delta : Derivative coefficients
        deltaErr[YAW] = errors[YAW] - previous_error[YAW];
        deltaErr[PITCH] = errors[PITCH] - previous_error[PITCH];
        deltaErr[ROLL] = errors[ROLL] - previous_error[ROLL];

        // Save current error as previous_error for next time
        previous_error[YAW] = errors[YAW];
        previous_error[PITCH] = errors[PITCH];
        previous_error[ROLL] = errors[ROLL];

        yaw = (errors[YAW] * Kp[YAW]) + (error_sum[YAW] * Ki[YAW]) + (deltaErr[YAW] * Kd[YAW]);
        pitch = (errors[PITCH] * Kp[PITCH]) + (error_sum[PITCH] * Ki[PITCH]) + (deltaErr[PITCH] * Kd[PITCH]);
        roll = (errors[ROLL] * Kp[ROLL]) + (error_sum[ROLL] * Ki[ROLL]) + (deltaErr[ROLL] * Kd[ROLL]);

        // Yaw - Lacet (Z axis)
        pulse_length_esc1 -= yaw;
        pulse_length_esc4 -= yaw;
        pulse_length_esc3 += yaw;
        pulse_length_esc2 += yaw;

        // Pitch - Tangage (Y axis)
        pulse_length_esc1 += pitch;
        pulse_length_esc2 += pitch;
        pulse_length_esc3 -= pitch;
        pulse_length_esc4 -= pitch;

        // Roll - Roulis (X axis)
        pulse_length_esc1 -= roll;
        pulse_length_esc3 -= roll;
        pulse_length_esc2 += roll;
        pulse_length_esc4 += roll;
    }

    pulse_length_esc1 = minMax(pulse_length_esc1, 1000, 2000);
    pulse_length_esc2 = minMax(pulse_length_esc2, 1000, 2000);
    pulse_length_esc3 = minMax(pulse_length_esc3, 1000, 2000);
    pulse_length_esc4 = minMax(pulse_length_esc4, 1000, 2000);
}


/**
 * Calculate errors of Yaw, Pitch & Roll: this is simply the difference between the measure and the command.
 *
 * @return void
 */
void calculateErrors() {
    errors[YAW]   = measures[YAW]   - instruction[YAW];
    errors[PITCH] = measures[PITCH] - instruction[PITCH];
    errors[ROLL]  = measures[ROLL]  - instruction[ROLL];
}

/**
 * Calculate real value of flight instructions from pulses length of each channel.
 *
 * - Roll     : from -33° to 33°
 * - Pitch    : from -33° to 33°
 * - Yaw      : from -180°/sec to 180°/sec
 * - Throttle : from 1000µs to 2000µs
 *
 * @return void
 */
void getFlightInstruction() {
    instruction[YAW]      = map(pulse_length[mode_mapping[YAW]], 1000, 2000, -180, 180);
    instruction[PITCH]    = map(pulse_length[mode_mapping[PITCH]], 1000, 2000, -33, 33);
    instruction[ROLL]     = map(pulse_length[mode_mapping[ROLL]], 1000, 2000, -33, 33);
    instruction[THROTTLE] = pulse_length[mode_mapping[THROTTLE]];
}

/**
 * Customize mapping of controls: set here which command is on which channel and call
 * this function in setup() routine.
 *
 * @return void
 */
void configureChannelMapping() {
    mode_mapping[YAW]      = CHANNEL4;
    mode_mapping[PITCH]    = CHANNEL2;
    mode_mapping[ROLL]     = CHANNEL1;
    mode_mapping[THROTTLE] = CHANNEL3;
}

/**
 * Configure gyro and accelerometer precision as following:
 *  - accelerometer: +/-8g
 *  - gyro: 500dps full scale
 *
 * @return void
 */
void setupMpu6050Registers() {
    //Activate the MPU-6050
    Wire.beginTransmission(MPU_ADDRESS);      // Start communicating with the MPU-6050
    Wire.write(0x6B);                         // Send the requested starting register
    Wire.write(0x00);                         // Set the requested starting register
    Wire.endTransmission();                   // End the transmission
    //Configure the accelerometer (+/-8g)
    Wire.beginTransmission(MPU_ADDRESS);      // Start communicating with the MPU-6050
    Wire.write(0x1C);                         // Send the requested starting register
    Wire.write(0x10);                         // Set the requested starting register
    Wire.endTransmission();                   // End the transmission
    //Configure the gyro (500dps full scale)
    Wire.beginTransmission(MPU_ADDRESS);      // Start communicating with the MPU-6050
    Wire.write(0x1B);                         // Send the requested starting register
    Wire.write(0x08);                         // Set the requested starting register
    Wire.endTransmission();                   // End the transmission
}

/**
 * Calibrate MPU6050: take 2000 samples to calculate average offsets.
 * During this step, the quadcopter needs to be static and on a horizontal surface.
 *
 * This function also sends low throttle signal to each ESC to init and prevent them beeping annoyingly.
 *
 * @return void
 */
void calibrateMpu6050()
{
    int max_samples = 2000;

    for (int i = 0; i < max_samples; i++) {
        readSensor();

        gyro_offset[X] += gyro_raw[X];
        gyro_offset[Y] += gyro_raw[Y];
        gyro_offset[Z] += gyro_raw[Z];

        // Generate low throttle pulse to init ESC and prevent them beeping
        PORTD |= B11110000;                  // Set pins #4 #5 #6 #7 HIGH
        delayMicroseconds(MIN_PULSE_LENGTH); // Wait 1000µs
        PORTD &= B00001111;                  // Then set LOW

        // Just wait a bit before next loop
        delay(3);
    }

    // Calculate average offsets
    gyro_offset[X] /= max_samples;
    gyro_offset[Y] /= max_samples;
    gyro_offset[Z] /= max_samples;
}

/**
 * Make sure that value is not over min_value/max_value.
 *
 * @param float value     : The value to convert
 * @param float min_value : The min value
 * @param float max_value : The max value
 * @return float
 */
float minMax(float value, float min_value, float max_value) {
    if (value > max_value) {
        value = max;
    } else if (value < min_value) {
        value = min_value;
    }

    return value;
}

/**
 * This Interrupt Sub Routine is called each time input 8, 9, 10 or 11 changed state.
 * Read the receiver signals in order to get flight instructions.
 *
 * This routine must be as fast as possible to prevent main program to be messed up.
 * The trick here is to use port registers to read pin state.
 * Doing (PINB & B00000001) is the same as digitalRead(8) with the advantage of using less CPU loops.
 * It is less convenient but more efficient, which is the most important here.
 *
 * @see https://www.arduino.cc/en/Reference/PortManipulation
 */
ISR(PCINT0_vect) {
    current_time = micros();

    // Channel 1 -------------------------------------------------
    if (PINB & B00000001) {                                        // Is input 8 high ?
        if (previous_state[CHANNEL1] == LOW) {                     // Input 8 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL1] = HIGH;                       // Save current state
            timer[CHANNEL1] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL1] == HIGH) {                 // Input 8 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL1] = LOW;                            // Save current state
        pulse_length[CHANNEL1] = current_time - timer[CHANNEL1];   // Calculate pulse duration & save it
    }

    // Channel 2 -------------------------------------------------
    if (PINB & B00000010) {                                        // Is input 9 high ?
        if (previous_state[CHANNEL2] == LOW) {                     // Input 9 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL2] = HIGH;                       // Save current state
            timer[CHANNEL2] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL2] == HIGH) {                 // Input 9 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL2] = LOW;                            // Save current state
        pulse_length[CHANNEL2] = current_time - timer[CHANNEL2];   // Calculate pulse duration & save it
    }

    // Channel 3 -------------------------------------------------
    if (PINB & B00000100) {                                        // Is input 10 high ?
        if (previous_state[CHANNEL3] == LOW) {                     // Input 10 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL3] = HIGH;                       // Save current state
            timer[CHANNEL3] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL3] == HIGH) {                 // Input 10 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL3] = LOW;                            // Save current state
        pulse_length[CHANNEL3] = current_time - timer[CHANNEL3];   // Calculate pulse duration & save it
    }

    // Channel 4 -------------------------------------------------
    if (PINB & B00001000) {                                        // Is input 11 high ?
        if (previous_state[CHANNEL4] == LOW) {                     // Input 11 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL4] = HIGH;                       // Save current state
            timer[CHANNEL4] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL4] == HIGH) {                 // Input 11 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL4] = LOW;                            // Save current state
        pulse_length[CHANNEL4] = current_time - timer[CHANNEL4];   // Calculate pulse duration & save it
    }
}
