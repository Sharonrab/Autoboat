// C standard library includes
#include <stdlib.h>
#include <stdio.h>

// Microchip standard library includes
#include <xc.h>
#include <pps.h>
#include <adc.h>
#include <uart.h>
#include <dma.h>
#include <timer.h>

// Project includes
#include "Node.h"
#include "MavlinkGlue.h"
#include "Acs300.h"
#include "Uart1.h"
#include "Uart2.h"
#include "Ecan1.h"
#include "PrimaryNode.h"
#include "DataStore.h"
#include "EcanSensors.h"
#include "Rudder.h"
#include "Actuators.h"
#include "MissionManager.h"
#include "Conversions.h"

// MATLAB-generate code includes
#include "controller.h"

// Define some macros for setting pins as inputs or outputs using the TRIS pins.
#define OUTPUT 0
#define INPUT 1

// Declare some macros for helping setting bit values
#define ON  1
#define OFF 0

// Specify how long after startup the node should stay in reset to let things stabilize. Units are
// centiseconds.
#define STARTUP_RESET_TIME 200

// Keep track of the processor's operating frequency.
#define F_OSC 80000000L

// Calculate the BRG register value necessary for 115200 baud with a 80MHz clock.
#define BAUD115200_BRG_REG 21

// Define the maximum value of the ADC input
#define ANmax 4095.0f

// Set the limit for when the GCS is considered disconnected at 30s. This leaves enough time for the
// chase boat to respond to when the first disconnection notice is received to prevent the SeaSlug
// from going into RTB mode.
#define GCS_DISCONNECTION_TIME 3000

// The time limit (in units of 0.01s) before a GPS disconnection state becomes an unrecoverable
// error.
#define GPS_DISCONNECTION_TIME 1000

// Store analog sensor data here

static struct {
    float powerRailVoltage;
    float powerRailCurrent;
} analogSensors;

// The trigger limit for when a STATUS_TEXT message should be sent with the current system status.
// Currently it's every 30s (so in units of 0.01s) after a waypoint has been hit. Additionally, this
// should trigger when switching to autonomous mode.
#define SAY_STATUS_COUNTER_LIMIT 3000
uint16_t sayStatusCounter = 0;

// This is used to store sensor availability from the last sample time in order to trigger on
// sensor availability changes.

static struct {
    bool gpsEnabled;
    bool gpsActive;
    bool imuEnabled;
    bool imuActive;
    bool wso100Enabled;
    bool wso100Active;
    bool dst800Enabled;
    bool dst800Active;
    bool powerEnabled;
    bool powerActive;
    bool propEnabled;
    bool propActive;
    bool rudderEnabled;
    bool rudderActive;
    bool rcNodeEnabled;
    bool rcNodeActive;
    bool gyroEnabled;
    bool gyroActive;
} lastSensorAvailability;

// Store actuator commmands here. Used by the MAVLink code.
ActuatorCommands currentCommands;

// Set up DMA memory for the ADC. But with the scatter- gather mode enabled on the ADC, we reserve
// an array for all 16 possible inputs, so we align to 32-byte boundaries instead.
#ifdef __dsPIC33FJ128MC802__
static volatile uint16_t adcDmaBuffer[16] __attribute__((space(dma), aligned(32)));
#elif __dsPIC33EP256MC502__
static volatile uint16_t adcDmaBuffer[16] __attribute__((aligned(32)));
#endif

// Declare some function prototypes.
void Adc1Init(void);
void PrimaryNode100HzLoop(void);
void PrimaryNodeMuxAndOutputControllerCommands(float rudderCommand, int16_t throttleCommand, bool forceTransmission);
void SetStatusModeLed(void);
void SetAutoModeLed(void);
void SetResetModeLed(void);
void SetGpsLed(void);
void TransmitNodeStatus2Hz(void);
float ProcessManualRudderCommand(float rc);
int16_t ProcessManualThrottleCommand(int16_t tc);
void ClearStateWhenErrors(void);
void SendAudioStatusUpdate(void);
void TransmitChannelUsage(void);

// Set processor configuration settings
#ifdef __dsPIC33FJ128MC802__
// Use internal RC to start; we then switch to PLL'd iRC.
_FOSCSEL(FNOSC_FRC & IESO_OFF);
// Clock Pragmas
_FOSC(FCKSM_CSECMD & OSCIOFNC_ON & POSCMD_NONE);
// Disable watchdog timer
_FWDT(FWDTEN_OFF);
// Disable JTAG and specify port 3 for ICD pins.
_FICD(JTAGEN_OFF & ICS_PGD3);
#elif __dsPIC33EP256MC502__
// Use internal RC to start; we then switch to PLL'd iRC.
_FOSCSEL(FNOSC_FRC & IESO_OFF);
// Clock Pragmas
_FOSC(FCKSM_CSECMD & OSCIOFNC_ON & POSCMD_NONE);
// Disable watchdog timer
_FWDT(FWDTEN_OFF);
// Disable JTAG and specify port 2 for ICD pins.
_FICD(JTAGEN_OFF & ICS_PGD2);
#endif

int main(void)
{
    /// First step is to move over to the FRC w/ PLL clock from the default FRC clock.
    // Set the clock to 79.84MHz
    PLLFBDbits.PLLDIV = 63; // M = 65
    CLKDIVbits.PLLPOST = 0; // N2 = 2
    CLKDIVbits.PLLPRE = 1; // N1 = 3
    // Set the clock to 140MHz (Kept for possible future use)
//    PLLFBDbits.PLLDIV = 74; // M = 76
//    CLKDIVbits.PLLPOST = 0; // N2 = 2
//    CLKDIVbits.PLLPRE = 0; // N1 = 2

    // Initiate Clock Switch to FRM oscillator with PLL.
    __builtin_write_OSCCONH(0x01);
    __builtin_write_OSCCONL(OSCCON | 0x01);

    // Wait for Clock switch to occur.
    while (OSCCONbits.COSC != 1);

    // And finally wait for the PLL to lock.
    while (OSCCONbits.LOCK != 1);

    // Set the ID for the primary node.
    nodeId = CAN_NODE_PRIMARY_CONTROLLER;

    // We aren't calculating CPU load for the primary controller
    nodeCpuLoad = UINT8_MAX;

    // Initialize UART1 to 115200 for groundstation communications.
    Uart1Init(BAUD115200_BRG_REG);

    // Initialize UART2 to 115200 for datalogger recording.
    Uart2Init(BAUD115200_BRG_REG);

    // Initialize the EEPROM for non-volatile data storage. DataStoreInit() also takes care of
    // initializing the onboard data store to the current parameter values so all subsequent calls
    // to DataStoreLoadAllParameters() should work.
    if (!DataStoreInit()) {
        FATAL_ERROR();
    }

    // Initialize ECAN1
    Ecan1Init(F_OSC, NODE_CAN_BAUD);

    // Set up the ADC
    Adc1Init();

    // Set up a timer at F_timer = F_OSC / 2 / 256.
    OpenTimer2(T2_ON & T2_IDLE_CON & T2_GATE_OFF & T2_PS_1_256 & T2_32BIT_MODE_OFF & T2_SOURCE_INT, UINT16_MAX);
    ConfigIntTimer2(T2_INT_PRIOR_1 & T2_INT_OFF);

    // Finally perform the necessary pin mappings:
    PPSUnLock;

#ifdef __dsPIC33FJ128MC802__
    // To enable ECAN1 pins: TX on 7, RX on 4
    PPSOutput(OUT_FN_PPS_C1TX, OUT_PIN_PPS_RP7);
    PPSInput(IN_FN_PPS_C1RX, IN_PIN_PPS_RP4);

    // To enable UART1 pins: TX on 11, RX on 13
    PPSOutput(OUT_FN_PPS_U1TX, OUT_PIN_PPS_RP11);
    PPSInput(IN_FN_PPS_U1RX, IN_PIN_PPS_RP13);
#elif __dsPIC33EP256MC502__
    // To enable ECAN1 pins: TX on 39, RX on 36
    PPSOutput(OUT_FN_PPS_C1TX, OUT_PIN_PPS_RP39);
    PPSInput(IN_FN_PPS_C1RX, IN_PIN_PPS_RP36);

    // To enable UART1 pins: TX on 43 (B11), RX on 45 (B13)
    PPSOutput(OUT_FN_PPS_U1TX, OUT_PIN_PPS_RP43);
    PPSInput(IN_FN_PPS_U1RX, IN_PIN_PPS_RPI45);

    // To enable UART2 pins: TX on 40 (B8), RX on 41 (B9)
    PPSOutput(OUT_FN_PPS_U2TX, OUT_PIN_PPS_RP40);
    PPSInput(IN_FN_PPS_U2RX, IN_PIN_PPS_RP41);
#endif
    PPSLock;

    // And specify input/output settings for digital I/O ports:
    // A3 (output): Red LED on the CANode, blinks at 2Hz when the system is in reset, and is solid when the system hit a fatal error, otherwise off.
    _TRISA3 = OUTPUT;
    // A4 (output): Amber LED on the CANode, blinks at 1Hz for status.
    _TRISA4 = OUTPUT;
    // B12 (output): Amber automode LED on the CANode Primary Shield, on when system is autonomous, blinking at 4Hz when in manual override, and off otherwise.
    _TRISB12 = OUTPUT;
    // B15 (output): Amber GPS LED on the CANode Primary Shield, on when GPS is active & receiving good data.
    _TRISB15 = OUTPUT;

    /// At this point all hardware has been initialized.

    // Initialize the MAVLink communications channel
    MavLinkInit();

    // Finally initialize the controller model (generated MATLAB code)
    controller_initialize();

    // Report on system status now that initialization is complete.
    MavLinkSendStatusText(MAV_SEVERITY_INFO, "Finished initialization.");

    // Alert the operator to how much bandwidth will be used for each channel
    TransmitChannelUsage();

    // Broadcast the current mission item. This is useful if the controller has been power-cycled.
    // In that case, if the system had been running off the built-in default missions, the only
    // thing that QGC will have out of date is the current mission item. This fixes that case. If
    // the waypoints are out-of-date, nothing can be done about that, as the mission protocol is a
    // pull-only protocol.
    MavLinkSendCurrentMission(0);

    // Track the last error state that we were in. Used for triggering events on changes
    static uint16_t lastErrorState = 0;

    // Run system tasks when a timer interrupt has been triggered.
    while (true) {

        // Process incoming ECAN messages. This is done continuously. We may get multiple of the
        // same message between our 100Hz primary controller ticks, so data may be overridden,
        // but that doesn't really matter.
        ProcessAllEcanMessages();

        // Check for any errors on the ECAN peripheral:
        EcanStatus ecanErrors = Ecan1GetErrorStatus();
        if (nodeStatus & PRIMARY_NODE_STATUS_ECAN_TX_ERR) {
            if (!ecanErrors.TxError && !ecanErrors.TxBufferOverflow) {
                nodeStatus &= ~PRIMARY_NODE_STATUS_ECAN_TX_ERR;
            }
        } else {
            if (ecanErrors.TxError || ecanErrors.TxBufferOverflow) {
                nodeStatus |= PRIMARY_NODE_STATUS_ECAN_TX_ERR;
            }
        }
        if (nodeStatus & PRIMARY_NODE_STATUS_ECAN_RX_ERR) {
            if (!ecanErrors.RxError && !ecanErrors.RxBufferOverflow) {
                nodeStatus &= ~PRIMARY_NODE_STATUS_ECAN_RX_ERR;
            }
        } else {
            if (ecanErrors.RxError || ecanErrors.RxBufferOverflow) {
                nodeStatus |= PRIMARY_NODE_STATUS_ECAN_RX_ERR;
            }
        }

        // Turn on the GPS indicator LED depending on the GPS status.
        if (lastSensorAvailability.gpsEnabled && !sensorAvailability.gps.enabled) {
            lastSensorAvailability.gpsEnabled = false;
        } else if (!lastSensorAvailability.gpsEnabled && sensorAvailability.gps.enabled) {
            lastSensorAvailability.gpsEnabled = true;
        }

        // Set the GPS invalid status bit when it's no longer active
        if (lastSensorAvailability.gpsActive && !sensorAvailability.gps.active) {
            nodeErrors |= PRIMARY_NODE_STATUS_GPS_INVALID;
            lastSensorAvailability.gpsActive = false;
        } else if (!lastSensorAvailability.gpsActive && sensorAvailability.gps.active) {
            nodeErrors &= ~PRIMARY_NODE_STATUS_GPS_INVALID;
            lastSensorAvailability.gpsActive = true;
        }

        // Set the GPS disconnected error bit when the GPS has been inactive for too long.
        if (nodeErrors & PRIMARY_NODE_RESET_GPS_DISCONNECTED) {
            if (sensorAvailability.gps.active) {
                nodeErrors &= ~PRIMARY_NODE_RESET_GPS_DISCONNECTED;
            }
        } else {
            if (nodeSystemTime - sensorAvailability.gps.last_active >= GPS_DISCONNECTION_TIME) {
                nodeErrors |= PRIMARY_NODE_RESET_GPS_DISCONNECTED;
            }
        }
        if (lastSensorAvailability.gpsActive && !sensorAvailability.gps.active) {
            nodeErrors |= PRIMARY_NODE_STATUS_GPS_INVALID;
            lastSensorAvailability.gpsActive = false;
        } else if (!lastSensorAvailability.gpsActive && sensorAvailability.gps.active) {
            nodeErrors &= ~PRIMARY_NODE_STATUS_GPS_INVALID;
            lastSensorAvailability.gpsActive = true;
        }

        // If we ever lose contact with the ACS300, assume it's an e-stop condition.
        if (lastSensorAvailability.propEnabled && !sensorAvailability.prop.enabled) {
            nodeErrors |= PRIMARY_NODE_RESET_ESTOP_OR_ACS300_DISCON;
            lastSensorAvailability.propEnabled = false;
        } else if (!lastSensorAvailability.propEnabled && sensorAvailability.prop.enabled) {
            nodeErrors &= ~PRIMARY_NODE_RESET_ESTOP_OR_ACS300_DISCON;
            lastSensorAvailability.propEnabled = true;
        }

        // And if the rudder node disconnects, set the uncalibrated reset line. There's no need to perform
        // the inverse check when it becomes active again, because that will be done when the CAN message
        // is received.
        if (lastSensorAvailability.rudderEnabled && !sensorAvailability.rudder.enabled) {
            nodeErrors |= PRIMARY_NODE_RESET_RUDDER_DISCONNECTED;
            lastSensorAvailability.rudderEnabled = false;
        } else if (!lastSensorAvailability.rudderEnabled && sensorAvailability.rudder.enabled) {
            nodeErrors &= ~PRIMARY_NODE_RESET_RUDDER_DISCONNECTED;
            lastSensorAvailability.rudderEnabled = true;
        }

        /// RC Node:
        // The RC node is considered enabled if it's broadcasting on the CAN bus. If the RC node
        // ever becomes disabled, we note it. This isn't worthy of a system error or triggering RTB,
        // but it should be noted in the logs.
        if (lastSensorAvailability.rcNodeEnabled && !sensorAvailability.rcNode.enabled) {
            nodeStatus |= PRIMARY_NODE_STATUS_RC_NODE_DISCONNECTED;
            lastSensorAvailability.rcNodeEnabled = false;
        } else if (!lastSensorAvailability.rcNodeEnabled && sensorAvailability.rcNode.enabled) {
            nodeStatus &= ~PRIMARY_NODE_STATUS_RC_NODE_DISCONNECTED;
            lastSensorAvailability.rcNodeEnabled = true;
        }

        /// DST800:
        // This is required for the water speed reading, so if it's disconnected, we enter a reset
        // state.
        if (lastSensorAvailability.dst800Enabled && !sensorAvailability.dst800.enabled) {
            nodeErrors |= PRIMARY_NODE_RESET_DST800_DISCONNECTED;
            lastSensorAvailability.dst800Enabled = false;
        } else if (!lastSensorAvailability.dst800Enabled && sensorAvailability.dst800.enabled) {
            nodeErrors &= ~PRIMARY_NODE_RESET_DST800_DISCONNECTED;
            lastSensorAvailability.dst800Enabled = true;
        }

        /// IMU:
        // This is required for heading & turn rate, so if it's disconnected, we enter a reset state.
        if (lastSensorAvailability.imuEnabled && !sensorAvailability.imu.enabled) {
            nodeErrors |= PRIMARY_NODE_RESET_IMU_DISCONNECTED;
            lastSensorAvailability.imuEnabled = false;
        } else if (!lastSensorAvailability.imuEnabled && sensorAvailability.imu.enabled) {
            nodeErrors &= ~PRIMARY_NODE_RESET_IMU_DISCONNECTED;
            lastSensorAvailability.imuEnabled = true;
        }

        // If the RC node has stopped being active, it means that manual control is no longer being
        // enforced, so we clear that status and re-transmit the latest autonomous control commands.
        // Otherwise if the RC node becomes active while it's also enabled, then we have manual
        // override.
        if (lastSensorAvailability.rcNodeActive && !sensorAvailability.rcNode.active) {
            nodeErrors &= ~PRIMARY_NODE_RESET_MANUAL_OVERRIDE;

            // Output the command messages for this timestep even if they haven't changed,
            // because the primary controller is now back in control of the vessel.
            // FIXME: The storing of this data should not be done in the
            // *OutputControllerCommands() function.
            PrimaryNodeMuxAndOutputControllerCommands(currentCommands.autonomousRudderCommand,
                                                      currentCommands.autonomousThrottleCommand,
                                                      true);
            lastSensorAvailability.rcNodeActive = false;
        } else if (sensorAvailability.rcNode.enabled &&
                   !lastSensorAvailability.rcNodeActive && sensorAvailability.rcNode.active) {
            nodeErrors |= PRIMARY_NODE_RESET_MANUAL_OVERRIDE;
            lastSensorAvailability.rcNodeActive = true;
        }

        // If the rudder is no longer active, then the rudder is either in an error state or it's
        // undergoing calibration. Calibration can only be done while in manual mode, so triggering
        // a reset state here won't affect manual control but will trigger RTB in autonomous mode,
        // which is what's desired.
        if (lastSensorAvailability.rudderActive && !sensorAvailability.rudder.active) {
            nodeErrors |= PRIMARY_NODE_RESET_RUDDER_ERRORS;
            lastSensorAvailability.rudderActive = false;
        } else if (sensorAvailability.rudder.enabled &&
                   !lastSensorAvailability.rudderActive && sensorAvailability.rudder.active) {
            nodeErrors &= ~PRIMARY_NODE_RESET_RUDDER_ERRORS;
            lastSensorAvailability.rudderActive = true;
        }

        // Track transitions in rudder calibrating state.
        if (nodeErrors & PRIMARY_NODE_RESET_CALIBRATING) {
            if (!rudderSensorData.Calibrating) {
                nodeErrors &= ~PRIMARY_NODE_RESET_CALIBRATING;
            }
        } else {
            if (rudderSensorData.Calibrating) {
                nodeErrors |= PRIMARY_NODE_RESET_CALIBRATING;
            }
        }
        // Track transitions in rudder calibrated state.
        if (nodeErrors & PRIMARY_NODE_RESET_UNCALIBRATED) {
            if (rudderSensorData.Calibrated) {
                nodeErrors &= ~PRIMARY_NODE_RESET_UNCALIBRATED;
            }
        } else {
            if (!rudderSensorData.Calibrated) {
                nodeErrors |= PRIMARY_NODE_RESET_UNCALIBRATED;
            }
        }

        // If the GCS has been disconnected for too long, set an error flag
        if (nodeErrors & PRIMARY_NODE_RESET_GCS_DISCONNECTED) {
            if (MavLinkTimeSinceLastGcsMessage() < GCS_DISCONNECTION_TIME) {
                nodeErrors &= ~PRIMARY_NODE_RESET_GCS_DISCONNECTED;
            }
        } else {
            if (MavLinkTimeSinceLastGcsMessage() >= GCS_DISCONNECTION_TIME) {
                nodeErrors |= PRIMARY_NODE_RESET_GCS_DISCONNECTED;
            }
        }

        // Check for new MAVLink messages. We may get multiple of the
        // same message between our 100Hz primary controller ticks, so data may be overridden,
        // but that doesn't really matter, as we were losing that data anyways when we were
        // calling it at 100Hz.
        MavLinkReceive();

        // At this point we check to see if we're in an error state. If this error state is
        // different than what we were in before, and it's one of the error states that should
        // trigger the return-to-base functionality, then we trigger RTB mode. This is currently
        // done by cutting the throttle, and centering the rudder.
        // Note that RTB mode is only engaged when the vehicle is autonomous, otherwise primary
        // control should be allowed in almost every circumstance.
        // Transmitting these commands is done whenever the error state changes tomake sure that
        // if the rudder or propeller subsystems go offline and back online that they'll receive
        // the message and hopefully respond properly.
        if (nodeErrors != lastErrorState) {
            const uint16_t rtbErrors = nodeErrors & RTB_RESET_MASK;
            if (IS_AUTONOMOUS() && rtbErrors) {
                ActuatorsTransmitCommands(0.0, 0, true);

                // Make sure the operator is aware we're in RTB mode, but only announce it once.
                if (!(nodeErrors & PRIMARY_NODE_RESET_RTB)) {
                    char audioStr[] = "Enacting return-to-base protocol (reason 0x0000)";
                    audioStr[43] = int2hexchar((rtbErrors >> 12) & 0xF);
                    audioStr[44] = int2hexchar((rtbErrors >> 8) & 0xF);
                    audioStr[45] = int2hexchar((rtbErrors >> 4) & 0xF);
                    audioStr[46] = int2hexchar(rtbErrors & 0xF);
                    MavLinkSendStatusText(MAV_SEVERITY_EMERGENCY, audioStr);

                    nodeErrors |= PRIMARY_NODE_RESET_RTB;
                }
            }
            lastErrorState = nodeErrors;
        }

        // Trigger the 100Hz loop when the timer counts past the 0.01s mark.
        if (TMR2 >= F_OSC / 2 / 256 / 100) {
            TMR2 = 0; // We need to reset the timer counter BEFORE doing anything in here or it
                      // throws off our calculations.
            PrimaryNode100HzLoop();
        }
    }
}

/**
 * Perform main timed loop at 100Hz.
 */
void PrimaryNode100HzLoop(void)
{
    // First update the status of any onboard sensors.
    UpdateSensorsAvailability();

    // Increment the counters for the mission and parameter protocols. They need to time some things
    // this way, as they're normally called as fast as possible, so this external counter method is
    // used.
    IncrementMissionCounter();
    IncrementParameterCounter();

    // Clear state on when errors
    ClearStateWhenErrors();

    // Check ADC inputs
    // Battery voltage in Volts
    analogSensors.powerRailVoltage = (3.3 / ANmax) / .06369 * (float)adcDmaBuffer[0];

    // Battery current in Amps
    analogSensors.powerRailCurrent = (3.3 / ANmax) / .03660 * (float)adcDmaBuffer[3];

    // Input voltage in dV (AN5)
    nodeVoltage = (uint8_t)((3.3 / ANmax) * ((21.0 + 2.0) / 2.0) * 10.0 * (float)adcDmaBuffer[5]);

    // Onboard temperature in degrees C (AN1)
    nodeTemp = (int8_t)((3.3 / ANmax * (float)adcDmaBuffer[1] - 0.5) * 100.0);

    // Set a reset signal for the first 2 seconds, allowing things to stabilize a bit before the
    // system responds. This is especially crucial because it can take up to a second for sensors to
    // timeout and appear as offline.
    if (nodeSystemTime == 0) {
        nodeErrors |= PRIMARY_NODE_RESET_STARTUP;
    } else if (nodeSystemTime == STARTUP_RESET_TIME) {
        nodeErrors &= ~PRIMARY_NODE_RESET_STARTUP;
    }

    // Update the LEDs
    SetResetModeLed();
    SetAutoModeLed();
    SetGpsLed();

    // Make sure we transmit NODE_STATUS messages at 2Hz.
    TransmitNodeStatus2Hz();

    // And make sure the primary LED is blinking indicating that the node is operational
    SetStatusModeLed();

    ImuData imu = {
        true,
        {(float)tokimecDataStore.yaw / 8192.0, (float)tokimecDataStore.pitch / 8192.0, (float)tokimecDataStore.roll / 8192.0},
        {(float)tokimecDataStore.x_angle_vel / 4096.0, (float)tokimecDataStore.y_angle_vel / 4096.0, (float)tokimecDataStore.z_angle_vel / 4096.0},
        {(float)tokimecDataStore.x_accel / 256.0, (float)tokimecDataStore.y_accel / 256.0, (float)tokimecDataStore.z_accel / 256.0}
    };

    // Copy all inputs to the controller here. This makes sure that what we transmit in the
    // CONTROLLER_DATA message is **exactly** what was computed on this timestep. Also we make sure
    // to set their input data to false here.
    // Note that this is actually unnecessary since the data can only change OUTSIDE of this 100Hz
    // event loop.
    bool reset = (nodeErrors != 0);
    GpsData controllerGpsIn;
    GetGpsData(&controllerGpsIn);
    float waterSpeed = GetWaterSpeed();
    float rudderAngle = rudderSensorData.RudderAngle;
    int16_t propSpeed = GetPropSpeed();

    // Run the control loop, the direct outputs are stored in the follow 2 variables. But note that
    // some additional system state is stored in controllerVars in `MavlinkGlue`.
    float rCommand = 0.0;
    int16_t tCommand = 0;
    controller_custom(&controllerGpsIn, &propSpeed, &rudderAngle, (boolean_T*)&reset, &waterSpeed,
        &rCommand, &tCommand, &controllerVars, &imu);

    // Now output the current system state and results of the last control loop, but at a reduced
    // rate of 50Hz.
    MavLinkSendControllerData(&imu, &controllerGpsIn, waterSpeed, rudderAngle, propSpeed, reset, rCommand, tCommand);

    // And output the necessary control outputs to the actuators. This will NOT transmit the commands
    // if the system is in a reset state. This can happen from errors, but also when the secondary
    // manual controller is active and controlling the vessel.
    PrimaryNodeMuxAndOutputControllerCommands(rCommand, tCommand, false);

    // If we've reached a new waypoint, announce to QGC that we have.
    if (controllerVars.wpReachedIndex != -1) {
        MavLinkSendMissionItemReached(controllerVars.wpReachedIndex);
    }

    // If we've switched to a new waypoint, announce to QGC that we have.
    if (controllerVars.wpCurrentIndex != -1) {
        MavLinkSendCurrentMission(controllerVars.wpCurrentIndex);
    }

    // Send any necessary groundstation messages for this timestep.
    MavLinkTransmitGroundstation();

    // Send any necessary datalogger messages for this timestep.
    MavLinkTransmitDatalogger();

    // Read out status updates when autonomous and not in an error state. The status counter is
    // reset when a waypoint is reached, otherwise we just count up and output the audio when the
    // counter limit is hit.
    if (IS_AUTONOMOUS() && !nodeErrors) {
        if (controllerVars.wpReachedIndex != -1) {
            sayStatusCounter = 0;
        } else if (sayStatusCounter >= SAY_STATUS_COUNTER_LIMIT) {
            SendAudioStatusUpdate();
            sayStatusCounter = 0;
        } else {
            ++sayStatusCounter;
        }
    }

    // Update the onboard system time counter. We make sure we don't overflow here as we can
    // run into issues with startup code being executed again.
    if (nodeSystemTime < UINT32_MAX) {
        ++nodeSystemTime;
    }
}

/**
 * Clear the GPS and Rudder internal data structures when the system goes into reset mode. This is
 * useful primarily when testing the primary controller and the node is left on through multiple
 * test runs.
 */
void ClearStateWhenErrors(void)
{
    static uint16_t lastErrorState = 0;

    if (!lastErrorState && nodeErrors) {
        ClearGpsData();
        ClearRudderAngle();
    }
    lastErrorState = nodeErrors;
}

/**
 * Set the primary status indicator LED to always blink
 */
void SetStatusModeLed(void)
{
    static uint8_t statusModeBlinkCounter = 0;

    if (statusModeBlinkCounter == 0) {
        _LATA4 = ON;
        statusModeBlinkCounter = 1;
    } else if (statusModeBlinkCounter == 100) {
        _LATA4 = OFF;
        ++statusModeBlinkCounter;
    } else if (statusModeBlinkCounter == 199) {
        statusModeBlinkCounter = 0;
    } else {
        ++statusModeBlinkCounter;
    }
}

/**
 * Set the reset indicator LED dependent on whether we are in a reset state or not
 */
void SetResetModeLed(void)
{
    static uint8_t resetModeBlinkCounter = 0;
    if (nodeErrors) {
        if (resetModeBlinkCounter == 0) {
            _LATA3 = ON;
            resetModeBlinkCounter = 1;
        } else if (resetModeBlinkCounter == 50) {
            _LATA3 = OFF;
            ++resetModeBlinkCounter;
        } else if (resetModeBlinkCounter == 99) {
            resetModeBlinkCounter = 0;
        } else {
            ++resetModeBlinkCounter;
        }
    } else {
        _LATA3 = OFF;
        resetModeBlinkCounter = 0;
    }
}

/**
 * Set the autonomous mode LED dependent on whether we are in autonomous mode, manual override,
 * or regular manual mode. LED is solid for autonomous mode, flashing for manual override, and
 * off for regular manual control.
 */
void SetAutoModeLed(void)
{
    static uint8_t autoModeBlinkCounter = 0;

    if (nodeErrors & PRIMARY_NODE_RESET_MANUAL_OVERRIDE) {
        if (autoModeBlinkCounter == 0) {
            _LATB12 = ON;
            autoModeBlinkCounter = 1;
        } else if (autoModeBlinkCounter == 25) {
            _LATB12 = OFF;
            ++autoModeBlinkCounter;
        } else if (autoModeBlinkCounter == 49) {
            autoModeBlinkCounter = 0;
        } else {
            ++autoModeBlinkCounter;
        }
    } else if (IS_AUTONOMOUS()) {
        _LATB12 = ON;
        autoModeBlinkCounter = 0;
    } else {
        _LATB12 = OFF;
        autoModeBlinkCounter = 0;
    }
}

/**
 * Set the GPS LED dependent on whether the GPS is enabled
 * or regular manual mode. LED is solid for autonomous mode, flashing for manual override, and
 * off for regular manual control.
 */
void SetGpsLed(void)
{
    static uint8_t gpsBlinkCounter = 0;

    // If the GPS is on and receiving good data, set the LED solid.
    if (sensorAvailability.gps.active) {
        _LATB15 = ON;
        gpsBlinkCounter = 0;
    }
    // But if the GPS isn't spitting out good data, but is still transmitting, blink the LED.
    else if (sensorAvailability.gps.enabled) {
        if (gpsBlinkCounter == 0) {
            _LATB15 = ON;
            gpsBlinkCounter = 1;
        } else if (gpsBlinkCounter == 25) {
            _LATB15 = OFF;
            ++gpsBlinkCounter;
        } else if (gpsBlinkCounter == 49) {
            gpsBlinkCounter = 0;
        } else {
            ++gpsBlinkCounter;
        }
    }
    // Otherwise no output.
    else {
        _LATB15 = OFF;
        gpsBlinkCounter = 0;
    }
}

/**
 * Transmit the node status message at 2Hz.
 */
void TransmitNodeStatus2Hz(void)
{
    static uint8_t counter = 0;
    if (counter == 49) {
        NodeTransmitStatus();
        ++counter;
    } else if (counter == 99) {
        NodeTransmitStatus();
        counter = 0;
    } else {
        ++counter;
    }
}

/**
 *
 * @param rudderCommand The autonomous controller's rudder command.
 * @param throttleCommand The autonomous throttle command. Currently unused because the controller doesn't command throttle, it will just hold whatever the last manual command was.
 * @param forceTransmission
 */
void PrimaryNodeMuxAndOutputControllerCommands(float rudderCommand, int16_t throttleCommand, bool forceTransmission)
{
    // Obtain and filter the manual control inputs
    float manRc;
    int16_t manTc;
    GetMavLinkManualControl(&manRc, &manTc);
    manRc = ProcessManualRudderCommand(manRc * 7.854e-4);
    manTc = ProcessManualThrottleCommand(manTc);
    currentCommands.primaryManualRudderCommand = manRc;
    currentCommands.primaryManualThrottleCommand = manTc;

    // Track autonomous actuator commands
    currentCommands.autonomousRudderCommand = rudderCommand;
    currentCommands.autonomousThrottleCommand = manTc; // Ignore autonomous throttle because there's no control loop around it. Just use manual setting instead.

    // Transmit the actuator commands if the system is autonomous and there're no errors.
    if (IS_AUTONOMOUS() && !nodeErrors) {
        // Throttle command is not managed by the autonomous controller yet, so just use the manually
        // set value.
        ActuatorsTransmitCommands(currentCommands.autonomousRudderCommand,
                                  currentCommands.autonomousThrottleCommand,
                                  forceTransmission);
    }
    // But allow manual control as long as the manual override isn't active.
    else if (!IS_AUTONOMOUS() && !(nodeErrors & PRIMARY_NODE_RESET_MANUAL_OVERRIDE)) {
        ActuatorsTransmitCommands(currentCommands.primaryManualRudderCommand,
                                  currentCommands.primaryManualThrottleCommand,
                                  forceTransmission);
    }
}

/**
 * Provides a helper function for retrieving the automode boolean value from the nodeStatus bitfield.
 * @returns A boolean of whether the vehicle is in autonomous mode or not.
 */
PrimaryNodeMode GetAutoMode(void)
{
    if ((nodeStatus & PRIMARY_NODE_STATUS_AUTOMODE) > 0) {
        return PRIMARY_MODE_AUTONOMOUS;
    } else {
        return PRIMARY_MODE_MANUAL;
    }
}

void SendAudioStatusUpdate(void)
{
    const int crosstrackErrorOffset = 12;
    const int waypointDistanceOffset = 37;
    char msg[50] = "#crosstrack      , waypoint distance      ";

    // Format the crosstrack error into a numerical XXXX.X format. Use "large" if it's too large
    float crosstrackError = CrossTrackError();
    if (crosstrackError < 10000) {
        uint16_t deciCrosstrackError = crosstrackError * 10.0;
        char cerrorStr[10];
        itoa(cerrorStr, deciCrosstrackError, 10);
        int cerrorStrLen = strlen(cerrorStr);
        if (cerrorStrLen == 1) { // If there's only 1 digit, special case everything for simplicity.
            msg[crosstrackErrorOffset] = '0';
            msg[crosstrackErrorOffset + 1] = '.';
            msg[crosstrackErrorOffset + 2] = cerrorStr[0];
        } else {
            memcpy(&msg[crosstrackErrorOffset], cerrorStr, cerrorStrLen - 1);
            msg[crosstrackErrorOffset + cerrorStrLen - 1] = '.';
            msg[crosstrackErrorOffset + cerrorStrLen] = cerrorStr[cerrorStrLen - 1];
        }
    } else {
        memcpy(&msg[crosstrackErrorOffset], "large", 5);
    }

    // Format the waypoint distance to a numerical XXXX format. Use "large" if it's too large
    uint16_t distance = DistanceToNextWaypoint();
    if (distance < 10000) {
        char distanceStr[10];
        itoa(distanceStr, distance, 10);
        memcpy(&msg[waypointDistanceOffset], distanceStr, strlen(distanceStr));
    } else {
        memcpy(&msg[waypointDistanceOffset], "large", 5);
    }

    MavLinkSendStatusText(MAV_SEVERITY_INFO, msg);
}

/**
 * Provides a helper function for updating the autonomous mode of the vehicle. This updates the
 * internal variable tracking the autonomous state of the controller. But it also does things based
 * on the change of state. When the vehicle is put into autonomous mode, it logs the current position
 * as the starting position to the 1st waypoint. It also outputs all parameters over MAVLink.
 *
 * Some conditions are checked before engaging autonomous mode, namely that there are now return-to-
 * base-triggering reset errors. This prevents the boat from unsafely entering autonomous mode,
 * though it can inhibit testing in some cases.
 *
 * This function also checks to see if the mode has changed before changing anything, so it is safely
 * idempotent.
 *
 * @param newMode True to put the vehicle into autonomous mode, False otherwise.
 */
void SetAutoMode(PrimaryNodeMode newMode)
{
    if (newMode == PRIMARY_MODE_AUTONOMOUS && !IS_AUTONOMOUS() && !(nodeErrors & RTB_RESET_MASK)) {
        // Update the vehicle state
        nodeStatus |= PRIMARY_NODE_STATUS_AUTOMODE;

        // Transmit a HEARTBEAT message to make sure the groundstation knows that we're autonomous
        MavLinkSendHeartbeat(MAVLINK_CHAN_GROUNDSTATION);
        MavLinkSendHeartbeat(MAVLINK_CHAN_DATALOGGER);

        // Send out an audio status update and reset the counter for another update every 30s.
        SendAudioStatusUpdate();
        sayStatusCounter = 0;

        // Also transmit all parameters so it's easy to verify the config of the vehicle later.
        MavLinkTransmitAllParameters();
    } else if (IS_AUTONOMOUS()) {
        // Update the vehicle state
        nodeStatus &= ~PRIMARY_NODE_STATUS_AUTOMODE;

        // And clear the RTB mode if it's enabled. Be sure to alert QGC if that's the case.
        if (nodeErrors & PRIMARY_NODE_RESET_RTB) {
            nodeErrors &= ~PRIMARY_NODE_RESET_RTB;

            // Make sure the operator is aware we're no longer in RTB mode
            MavLinkSendStatusText(MAV_SEVERITY_NOTICE, "Exiting return-to-base protocol");
        }

        // Transmit a HEARTBEAT message to make sure the groundstation knows that we're now manual
        MavLinkSendHeartbeat(MAVLINK_CHAN_GROUNDSTATION);
        MavLinkSendHeartbeat(MAVLINK_CHAN_DATALOGGER);
    }
}

/**
 * Perform a bunch of processing on a manual rudder angle input including binning the final value.
 * @param rc A rudder angle in radians.
 */
float ProcessManualRudderCommand(float rc)
{
    // Set up 9 bins for binning the rudder command into, broken into (degrees):
    // 0-6, 6-12, 12-18, 18-23, 23-28, 28-33, 33-39, 39-45
    static const int numBins = 9;
    static const float transitions[] = {0.0, 6 * M_PI / 180, 11 * M_PI / 180, 16 * M_PI / 180, 21 * M_PI / 180, 26 * M_PI / 180, 31 * M_PI / 180, 36 * M_PI / 180, 40 * M_PI / 180};
    static const float binAngles[] = {0.0, 6 * M_PI / 180, 12 * M_PI / 180, 18 * M_PI / 180, 23 * M_PI / 180, 28 * M_PI / 180, 33 * M_PI / 180, 39 * M_PI / 180, 45 * M_PI / 180};

    static int bin = 0; // Track the current bin we're in.
    static float lastRc; // The last received control command

    // Cap the input to +- 45 degrees.
    if (rc > 0.7854) {
        rc = 0.7854;
    } else if (rc < -0.7854) {
        rc = -0.7854;
    }

    // Now average with the last sample
    rc = (rc + lastRc) / 2.0;
    lastRc = rc;

    // Attempt to move up a bin taking into account hysteresis.
    if (bin + 1 < numBins) {
        if (fabs(rc) > (transitions[bin + 1] + 0.0349)) {
            ++bin;
        }
    }

    // Attempt to step down a bin taking into account hysteresis.
    if (bin - 1 >= 0) {
        if (fabs(rc) < (transitions[bin] - 0.0436)) {
            --bin;
        }
    }

    // Finally return the mapped value accounting for sign.
    if (rc < 0) {
        return -binAngles[bin];
    } else {
        return binAngles[bin];
    }
}

/**
 * Perform a bunch of processing on a manual throttle input including binning the final value.
 * @param tc The commanded throttle in units from -1000 (full reverse) to 1000 (full forward)
 */
int16_t ProcessManualThrottleCommand(int16_t tc)
{
    // Add an 8% deadband
    if (tc > -40 && tc < 40) {
        tc = 0;
    }

    // Scale the throttle command to only 70%. This is to give more control to the operator and
    // won't reduce their control. The integer math of (x*7/10) shouldn't overflow for the input
    // ranges this function assumes.
    return tc * 7 / 10;
}

/**
 * Configure ADC1 for reading in 4 channels:
 * AN0 - Power rail voltage
 * AN1 - Analog temperature sensor (TC1047)
 * AN3 - Power rail current
 * AN5 - Input voltage (voltage divider)
 * Relies on DMA1
 */
void Adc1Init(void)
{
    // Initialize ADC for reading temperature, 2x power rail voltage, and current draw.
    // Use standard V_ref+/V_ref- voltage references.
    SetChanADC1(
            ADC_CH123_NEG_SAMPLEA_VREFN & ADC_CH123_NEG_SAMPLEB_VREFN & ADC_CH123_POS_SAMPLEA_0_1_2 & ADC_CH123_POS_SAMPLEB_0_1_2,
            ADC_CH0_POS_SAMPLEA_AN0 & ADC_CH0_NEG_SAMPLEA_VREFN & ADC_CH0_POS_SAMPLEB_AN0 & ADC_CH0_NEG_SAMPLEB_VREFN
            );
    // Open AN1 (temperature) and AN5 (voltage) pins for 12-bit unsigned integer readings. Also note
    // that this will only store one sample per analog input.
#ifdef __dsPIC33FJ128MC802__
    OpenADC1(
            ADC_MODULE_ON & ADC_IDLE_CONTINUE & ADC_ADDMABM_SCATTR & ADC_AD12B_12BIT & ADC_FORMAT_INTG & ADC_CLK_AUTO & ADC_AUTO_SAMPLING_ON & ADC_SIMULTANEOUS & ADC_SAMP_ON,
            ADC_VREF_AVDD_AVSS & ADC_SCAN_ON & ADC_SELECT_CHAN_0 & ADC_DMA_ADD_INC_4 & ADC_ALT_BUF_OFF & ADC_ALT_INPUT_OFF,
            ADC_SAMPLE_TIME_31 & ADC_CONV_CLK_INTERNAL_RC & ADC_CONV_CLK_32Tcy,
            ADC_DMA_BUF_LOC_1,
            ENABLE_AN0_ANA & ENABLE_AN1_ANA & ENABLE_AN3_ANA & ENABLE_AN5_ANA,
            ENABLE_ALL_DIG_16_31,
            SCAN_NONE_16_31,
            SKIP_SCAN_AN2 & SKIP_SCAN_AN4 & SKIP_SCAN_AN6 & SKIP_SCAN_AN7 &
            SKIP_SCAN_AN8 & SKIP_SCAN_AN9 & SKIP_SCAN_AN10 & SKIP_SCAN_AN11 & SKIP_SCAN_AN12 & SKIP_SCAN_AN13 &
            SKIP_SCAN_AN14 & SKIP_SCAN_AN15
            );
#elif __dsPIC33EP256MC502__
    OpenADC1(
            ADC_MODULE_ON & ADC_IDLE_CONTINUE & ADC_ADDMABM_SCATTR & ADC_AD12B_12BIT & ADC_FORMAT_INTG & ADC_SSRC_AUTO & ADC_AUTO_SAMPLING_ON & ADC_SIMULTANEOUS & ADC_SAMP_ON,
            ADC_VREF_AVDD_AVSS & ADC_SCAN_ON & ADC_SELECT_CHAN_0 & ADC_ALT_BUF_OFF & ADC_ALT_INPUT_OFF,
            ADC_SAMPLE_TIME_31 & ADC_CONV_CLK_INTERNAL_RC & ADC_CONV_CLK_32Tcy,
            ADC_DMA_BUF_LOC_1,
            0x1 | 0x2, // Read AN0/A0 (voltage sense) and AN1/A1 (temperature)
            0x2 | 0x8, // Read AN3/B1 (current sense) and AN5/B3 (board voltage sense)
            0, // Don't read any pins in portc
            0, // Don't read any pins in portd
            0, // Don't read any pins in porte
            0, // Don't read any pins in portf
            0, // Don't read any pins in portg
            0, // Don't read any pins in porth
            0, // Don't read any pins in porti
            0, // Don't read any pins in portj
            0, // Don't read any pins in portk
            SCAN_NONE_16_31,
            SKIP_SCAN_AN2 & SKIP_SCAN_AN4 & SKIP_SCAN_AN6 & SKIP_SCAN_AN7 &
            SKIP_SCAN_AN8 & SKIP_SCAN_AN9 & SKIP_SCAN_AN10 & SKIP_SCAN_AN11 & SKIP_SCAN_AN12 & SKIP_SCAN_AN13 &
            SKIP_SCAN_AN14 & SKIP_SCAN_AN15
            );
#endif
    // Open DMA1 for receiving ADC values
    OpenDMA1(DMA1_MODULE_ON & DMA1_SIZE_WORD & PERIPHERAL_TO_DMA1 & DMA1_INTERRUPT_BLOCK & DMA1_NORMAL & DMA1_PERIPHERAL_INDIRECT & DMA1_CONTINUOUS,
             0x0D,
#ifdef __dsPIC33FJ128MC802__
            __builtin_dmaoffset(adcDmaBuffer),
#elif __dsPIC33EP256MC502__
            (unsigned long int)adcDmaBuffer, // Warning here (cast from pointer to integer of different size) expected, unknown how to fix
#endif
            0ul,
            (uint16_t) & ADC1BUF0,
            3); // Specify the number of pins being measured (n) as n-1 here. Must match ADC_DMA_ADD_INC_n setting.
}

float GetPowerRailVoltage(void)
{
    return analogSensors.powerRailVoltage;
}

float GetPowerRailCurrent(void)
{
    return analogSensors.powerRailCurrent;
}

void GetCurrentActuatorCommands(float *rudderAngle, int16_t *throttle)
{
    // If we've detected a manual override from the RC node, that's what's driving the boat.
    if (nodeErrors & PRIMARY_NODE_RESET_MANUAL_OVERRIDE) {
        *rudderAngle = currentCommands.secondaryManualRudderCommand;
        *throttle = currentCommands.secondaryManualThrottleCommand;
    }
    // Otherwise, if there aren't any other errors, it depends on the mode.
    else if (!nodeErrors) {
        if (IS_AUTONOMOUS()) {
            *rudderAngle = currentCommands.autonomousRudderCommand;
            *throttle = currentCommands.autonomousThrottleCommand;
        } else {
            *rudderAngle = currentCommands.primaryManualRudderCommand;
            *throttle = currentCommands.primaryManualThrottleCommand;
        }
    }
    // There's nothing driving the boat in this case
    else {
        *rudderAngle = 0.0;
        *throttle = 0;
    }
}

int16_t BearingToNextWaypoint(void)
{
    // Calculate the desired bearing of the current waypoint segment
    const float distNorth = controllerVars.wp1[0] - controllerVars.LocalPosition[0];
    const float distEast = controllerVars.wp1[1] - controllerVars.LocalPosition[1];
    const float waypointAbsBearing = atan2(distEast, distNorth);

    // And calculate the difference from the current heading of the boat
    const float vesselBearing = controllerVars.Course;
    const float bearing = (waypointAbsBearing - vesselBearing) * 180.0 / M_PI;

    return (int16_t)bearing;
}

uint16_t DistanceToNextWaypoint(void)
{
    const float distNorth = controllerVars.wp1[0] - controllerVars.LocalPosition[0];
    const float distEast = controllerVars.wp1[1] - controllerVars.LocalPosition[1];

    return (uint16_t)sqrt(distNorth * distNorth + distEast * distEast);
}

float CrossTrackError(void)
{
    const float *p1 = controllerVars.wp0;
    const float *p2 = controllerVars.wp1;
    const float *p0 = controllerVars.LocalPosition; // The vehicle's position

    // Calculate the denominator, which is just the distance between the waypoints
    const float denominator = sqrtf((p2[1] - p1[1]) * (p2[1] - p1[1]) +
                                       (p2[0] - p1[0]) * (p2[0] - p1[0]));

    // And calculate the numerator
    const float numerator = fabs((p2[0] - p1[0]) * p0[1] - (p2[1] - p1[1]) * p0[0] + p2[1] * p1[0] -
        p2[0] * p1[1]);

    // Make sure the value is calculatable.
    if (denominator != 0.0) {
        return numerator / denominator;
    } else {
        return NAN;
    }
}

void TransmitChannelUsage(void)
{
    // Get the percentage utilization of the groundstation channel
    uint8_t gsChannelUsage = MavLinkGetChannelUsage(MAVLINK_CHAN_GROUNDSTATION);

    // Stringify it
    char gsUsageString[] = "Groundstation channel usage at   0%";
    if (gsChannelUsage >= 100) {
        gsUsageString[31] = int2hexchar(gsChannelUsage / 100);
        gsChannelUsage %= 100;
    }
    gsUsageString[32] = int2hexchar(gsChannelUsage / 10);
    gsChannelUsage %= 10;
    gsUsageString[33] = int2hexchar(gsChannelUsage);

    // And transmit!
    MavLinkSendStatusText(MAV_SEVERITY_INFO, gsUsageString);

    // Get the percentage utilization of the groundstation channel
    uint8_t dlChannelUsage = MavLinkGetChannelUsage(MAVLINK_CHAN_DATALOGGER);

    // Stringify it
    char dlUsageString[] = "Datalogger channel usage at   0%";
    if (dlChannelUsage >= 100) {
        dlUsageString[28] = int2hexchar(dlChannelUsage / 100);
        dlChannelUsage %= 100;
    }
    dlUsageString[29] = int2hexchar(dlChannelUsage / 10);
    dlChannelUsage %= 10;
    dlUsageString[30] = int2hexchar(dlChannelUsage);

    // And transmit!
    MavLinkSendStatusText(MAV_SEVERITY_INFO, dlUsageString);
}
