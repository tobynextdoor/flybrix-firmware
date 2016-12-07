/*
    *  Flybrix Flight Controller -- Copyright 2015 Flying Selfie Inc.
    *
    *  License and other details available at: http://www.flybrix.com/firmware
*/

#include "serial.h"

#include "serialFork.h"
#include "state.h"

#include "cardManagement.h"
#include "command.h"
#include "config.h"  //CONFIG variable
#include "control.h"
#include "led.h"
#include "systems.h"

namespace {
using CobsPayloadGeneric = CobsPayload<1000>;  // impacts memory use only; packet size should be <= client packet size

template <std::size_t N>
inline void WriteProtocolHead(SerialComm::MessageType type, uint32_t mask, CobsPayload<N>& payload) {
    payload.Append(type);
    payload.Append(mask);
}

template <std::size_t N>
inline void WriteToOutput(CobsPayload<N>& payload, bool use_logger = false) {
    auto package = payload.Encode();
    if (use_logger)
        sdcard::write(package.data, package.length);
    else
        writeSerial(package.data, package.length);
}

template <std::size_t N>
inline void WritePIDData(CobsPayload<N>& payload, const PID& pid) {
    payload.Append(pid.lastTime(), pid.input(), pid.setpoint(), pid.pTerm(), pid.iTerm(), pid.dTerm());
}
}

SerialComm::SerialComm(State* state, const volatile uint16_t* ppm, const Control* control, Systems* systems, LED* led, PilotCommand* command)
    : state{state}, ppm{ppm}, control{control}, systems{systems}, led{led}, command{command} {
}

void SerialComm::Read() {
    for (;;) {
        CobsReaderBuffer* buffer{readSerial()};
        if (buffer == nullptr)
            return;
        ProcessData(*buffer);
    }
}

void SerialComm::ProcessData(CobsReaderBuffer& data_input) {
    MessageType code;
    uint32_t mask;

    if (!data_input.ParseInto(code, mask))
        return;
    if (code != MessageType::Command)
        return;

    uint32_t ack_data{0};

    if (mask & COM_SET_EEPROM_DATA) {
        CONFIG_union tmp_config;
        if (data_input.ParseInto(tmp_config.raw)) {
            if (tmp_config.data.verify()) {
                tmp_config.data.applyTo(*systems);
                writeEEPROM(tmp_config);  // TODO: deal with side effect code
                ack_data |= COM_SET_EEPROM_DATA;
            }
        }
    }
    if (mask & COM_REINIT_EEPROM_DATA) {
        CONFIG_union tmp_config;
        tmp_config.data.applyTo(*systems);
        writeEEPROM(tmp_config);  // TODO: deal with side effect code
        ack_data |= COM_REINIT_EEPROM_DATA;
    }
    if (mask & COM_REQ_EEPROM_DATA) {
        SendConfiguration();
        ack_data |= COM_REQ_EEPROM_DATA;
    }
    if (mask & COM_REQ_ENABLE_ITERATION) {
        uint8_t flag;
        if (data_input.ParseInto(flag)) {
            if (flag == 1)
                state->processMotorEnablingIteration();
            else
                state->disableMotors();
            ack_data |= COM_REQ_ENABLE_ITERATION;
        }
    }
    // This should pass if any motor speed is set
    if (mask & COM_MOTOR_OVERRIDE_SPEED_ALL) {
        if (mask & COM_MOTOR_OVERRIDE_SPEED_0 && data_input.ParseInto(state->MotorOut[0]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_0;
        if (mask & COM_MOTOR_OVERRIDE_SPEED_1 && data_input.ParseInto(state->MotorOut[1]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_1;
        if (mask & COM_MOTOR_OVERRIDE_SPEED_2 && data_input.ParseInto(state->MotorOut[2]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_2;
        if (mask & COM_MOTOR_OVERRIDE_SPEED_3 && data_input.ParseInto(state->MotorOut[3]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_3;
        if (mask & COM_MOTOR_OVERRIDE_SPEED_4 && data_input.ParseInto(state->MotorOut[4]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_4;
        if (mask & COM_MOTOR_OVERRIDE_SPEED_5 && data_input.ParseInto(state->MotorOut[5]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_5;
        if (mask & COM_MOTOR_OVERRIDE_SPEED_6 && data_input.ParseInto(state->MotorOut[6]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_6;
        if (mask & COM_MOTOR_OVERRIDE_SPEED_7 && data_input.ParseInto(state->MotorOut[7]))
            ack_data |= COM_MOTOR_OVERRIDE_SPEED_7;
    }
    if (mask & COM_SET_COMMAND_OVERRIDE) {
        uint8_t flag;
        if (data_input.ParseInto(flag)) {
            if (flag == 1)
                state->set(STATUS_OVERRIDE);
            else
                state->clear(STATUS_OVERRIDE);
            ack_data |= COM_SET_COMMAND_OVERRIDE;
        }
    }
    if (mask & COM_SET_STATE_MASK) {
        uint32_t new_state_mask;
        if (data_input.ParseInto(new_state_mask)) {
            SetStateMsg(new_state_mask);
            ack_data |= COM_SET_STATE_MASK;
        }
    }
    if (mask & COM_SET_STATE_DELAY) {
        uint16_t new_state_delay;
        if (data_input.ParseInto(new_state_delay)) {
            send_state_delay = new_state_delay;
            ack_data |= COM_SET_STATE_DELAY;
        }
    }
    if (mask & COM_SET_SD_WRITE_DELAY) {
        uint16_t new_state_delay;
        if (data_input.ParseInto(new_state_delay)) {
            sd_card_state_delay = new_state_delay;
            ack_data |= COM_SET_SD_WRITE_DELAY;
        }
    }
    if (mask & COM_SET_LED) {
        uint8_t mode, r1, g1, b1, r2, g2, b2, ind_r, ind_g;
        if (data_input.ParseInto(mode, r1, g1, b1, r2, g2, b2, ind_r, ind_g)) {
            led->set(LED::Pattern(mode), r1, g1, b1, r2, g2, b2, ind_r, ind_g);
            ack_data |= COM_SET_LED;
        }
    }
    if (mask & COM_SET_SERIAL_RC) {
        uint8_t enabled;
        int16_t throttle, pitch, roll, yaw;
        uint8_t auxmask;
        if (data_input.ParseInto(enabled, throttle, pitch, roll, yaw, auxmask)) {
            if (enabled) {
                state->command_source_mask |= COMMAND_READY_BTLE;
                state->command_AUX_mask = auxmask;
                state->command_throttle = throttle;
                state->command_pitch = pitch;
                state->command_roll = roll;
                state->command_yaw = yaw;
            } else {
                state->command_source_mask &= ~COMMAND_READY_BTLE;
            }
            ack_data |= COM_SET_SERIAL_RC;
        }
    }
    if (mask & COM_SET_CARD_RECORDING) {
        uint8_t recording_flags;
        if (data_input.ParseInto(recording_flags)) {
            bool shouldRecordToCard = recording_flags & 1;
            bool shouldLock = recording_flags & 2;
            sdcard::setLock(false);
            if (shouldRecordToCard)
                sdcard::openFile();
            else
                sdcard::closeFile();
            sdcard::setLock(shouldLock);
            ack_data |= COM_SET_CARD_RECORDING;
        }
    }
    if (mask & COM_SET_PARTIAL_EEPROM_DATA) {
        uint16_t submask;
        if (data_input.ParseInto(submask)) {
            CONFIG_union tmp_config(*systems);
            bool success{true};
            if (success && (submask & CONFIG_struct::VERSION)) {
                success = data_input.ParseInto(tmp_config.data.version);
            }
            if (success && (submask & CONFIG_struct::PCB)) {
                success = data_input.ParseInto(tmp_config.data.pcb);
            }
            if (success && (submask & CONFIG_struct::MIX_TABLE)) {
                success = data_input.ParseInto(tmp_config.data.mix_table);
            }
            if (success && (submask & CONFIG_struct::MAG_BIAS)) {
                success = data_input.ParseInto(tmp_config.data.mag_bias);
            }
            if (success && (submask & CONFIG_struct::CHANNEL)) {
                success = data_input.ParseInto(tmp_config.data.channel);
            }
            if (success && (submask & CONFIG_struct::PID_PARAMETERS)) {
                success = data_input.ParseInto(tmp_config.data.pid_parameters);
            }
            if (success && (submask & CONFIG_struct::STATE_PARAMETERS)) {
                success = data_input.ParseInto(tmp_config.data.state_parameters);
            }
            if (success && (submask & CONFIG_struct::LED_STATES)) {
                // split up LED states further, since the variable is giant
                uint16_t led_mask;
                success = data_input.ParseInto(led_mask);
                for (size_t led_code = 0; success && (led_code < 16); ++led_code) {
                    if (led_mask & (1 << led_code)) {
                        success = data_input.ParseInto(tmp_config.data.led_states.states[led_code]);
                    }
                }
            }
            if (submask & CONFIG_struct::DEVICE_NAME) {
                success = data_input.ParseInto(tmp_config.data.name);
            }
            if (success && tmp_config.data.verify()) {
                tmp_config.data.applyTo(*systems);
                writeEEPROM(tmp_config);  // TODO: deal with side effect code
                ack_data |= COM_SET_PARTIAL_EEPROM_DATA;
            }
        }
    }
    if (mask & COM_REINIT_PARTIAL_EEPROM_DATA) {
        uint16_t submask;
        if (data_input.ParseInto(submask)) {
            CONFIG_union tmp_config(*systems);
            CONFIG_union default_config;
            bool success{true};
            if (submask & CONFIG_struct::VERSION) {
                tmp_config.data.version = default_config.data.version;
            }
            if (submask & CONFIG_struct::PCB) {
                tmp_config.data.pcb = default_config.data.pcb;
            }
            if (submask & CONFIG_struct::MIX_TABLE) {
                tmp_config.data.mix_table = default_config.data.mix_table;
            }
            if (submask & CONFIG_struct::MAG_BIAS) {
                tmp_config.data.mag_bias = default_config.data.mag_bias;
            }
            if (submask & CONFIG_struct::CHANNEL) {
                tmp_config.data.channel = default_config.data.channel;
            }
            if (submask & CONFIG_struct::PID_PARAMETERS) {
                tmp_config.data.pid_parameters = default_config.data.pid_parameters;
            }
            if (submask & CONFIG_struct::STATE_PARAMETERS) {
                tmp_config.data.state_parameters = default_config.data.state_parameters;
            }
            if (submask & CONFIG_struct::LED_STATES) {
                uint16_t led_mask;
                success = data_input.ParseInto(led_mask);
                for (size_t led_code = 0; success && (led_code < 16); ++led_code) {
                    if (led_mask & (1 << led_code)) {
                        tmp_config.data.led_states.states[led_code] = default_config.data.led_states.states[led_code];
                    }
                }
            }
            if (success && tmp_config.data.verify()) {
                tmp_config.data.applyTo(*systems);
                writeEEPROM(tmp_config);  // TODO: deal with side effect code
                ack_data |= COM_REINIT_PARTIAL_EEPROM_DATA;
            }
        }
    }
    if (mask & COM_REQ_PARTIAL_EEPROM_DATA) {
        uint16_t submask;
        if (data_input.ParseInto(submask)) {
            uint16_t led_mask{0};
            if (!(submask & CONFIG_struct::LED_STATES) || data_input.ParseInto(led_mask)) {
                SendPartialConfiguration(submask, led_mask);
                ack_data |= COM_REQ_PARTIAL_EEPROM_DATA;
            }
        }
    }
    if (mask & COM_REQ_CARD_RECORDING_STATE) {
        CobsPayload<20> payload;
        WriteProtocolHead(SerialComm::MessageType::Command, COM_SET_SD_WRITE_DELAY | COM_SET_CARD_RECORDING, payload);
        payload.Append(sd_card_state_delay);
        uint8_t flags = 0;
        if (sdcard::isOpen()) {
            flags |= 1;
        }
        if (sdcard::isLocked()) {
            flags |= 2;
        }
        payload.Append(flags);
        WriteToOutput(payload);
        ack_data |= COM_REQ_CARD_RECORDING_STATE;
    }

    if (mask & COM_REQ_RESPONSE) {
        SendResponse(mask, ack_data);
    }
}

void SerialComm::SendConfiguration() const {
    CobsPayloadGeneric payload;
    WriteProtocolHead(SerialComm::MessageType::Command, COM_SET_EEPROM_DATA, payload);
    payload.Append(CONFIG_struct(*systems));
    WriteToOutput(payload);
}

void SerialComm::SendPartialConfiguration(uint16_t submask, uint16_t led_mask) const {
    CobsPayloadGeneric payload;
    WriteProtocolHead(SerialComm::MessageType::Command, COM_SET_PARTIAL_EEPROM_DATA, payload);

    CONFIG_union tmp_config(*systems);
    payload.Append(submask);
    if (submask & CONFIG_struct::VERSION) {
        payload.Append(tmp_config.data.version);
    }
    if (submask & CONFIG_struct::PCB) {
        payload.Append(tmp_config.data.pcb);
    }
    if (submask & CONFIG_struct::MIX_TABLE) {
        payload.Append(tmp_config.data.mix_table);
    }
    if (submask & CONFIG_struct::MAG_BIAS) {
        payload.Append(tmp_config.data.mag_bias);
    }
    if (submask & CONFIG_struct::CHANNEL) {
        payload.Append(tmp_config.data.channel);
    }
    if (submask & CONFIG_struct::PID_PARAMETERS) {
        payload.Append(tmp_config.data.pid_parameters);
    }
    if (submask & CONFIG_struct::STATE_PARAMETERS) {
        payload.Append(tmp_config.data.state_parameters);
    }
    if (submask & CONFIG_struct::LED_STATES) {
        payload.Append(led_mask);
        for (size_t led_code = 0; led_code < 16; ++led_code) {
            if (led_mask & (1 << led_code)) {
                payload.Append(tmp_config.data.led_states.states[led_code]);
            }
        }
    }
    if (submask & CONFIG_struct::DEVICE_NAME) {
        payload.Append(tmp_config.data.name);
    }

    WriteToOutput(payload);
}

void SerialComm::SendDebugString(const String& string, MessageType type) const {
    CobsPayload<2000> payload;
    WriteProtocolHead(type, 0xFFFFFFFF, payload);
    size_t str_len = string.length();
    for (size_t i = 0; i < str_len; ++i)
        payload.Append(string.charAt(i));
    payload.Append(uint8_t(0));
    WriteToOutput(payload);
}

uint16_t SerialComm::PacketSize(uint32_t mask) const {
    uint16_t sum = 0;
    if (mask & SerialComm::STATE_MICROS)
        sum += 4;
    if (mask & SerialComm::STATE_STATUS)
        sum += 2;
    if (mask & SerialComm::STATE_V0)
        sum += 2;
    if (mask & SerialComm::STATE_I0)
        sum += 2;
    if (mask & SerialComm::STATE_I1)
        sum += 2;
    if (mask & SerialComm::STATE_ACCEL)
        sum += 3 * 4;
    if (mask & SerialComm::STATE_GYRO)
        sum += 3 * 4;
    if (mask & SerialComm::STATE_MAG)
        sum += 3 * 4;
    if (mask & SerialComm::STATE_TEMPERATURE)
        sum += 2;
    if (mask & SerialComm::STATE_PRESSURE)
        sum += 4;
    if (mask & SerialComm::STATE_RX_PPM)
        sum += 6 * 2;
    if (mask & SerialComm::STATE_AUX_CHAN_MASK)
        sum += 1;
    if (mask & SerialComm::STATE_COMMANDS)
        sum += 4 * 2;
    if (mask & SerialComm::STATE_F_AND_T)
        sum += 4 * 4;
    if (mask & SerialComm::STATE_PID_FZ_MASTER)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_PID_TX_MASTER)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_PID_TY_MASTER)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_PID_TZ_MASTER)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_PID_FZ_SLAVE)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_PID_TX_SLAVE)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_PID_TY_SLAVE)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_PID_TZ_SLAVE)
        sum += 7 * 4;
    if (mask & SerialComm::STATE_MOTOR_OUT)
        sum += 8 * 2;
    if (mask & SerialComm::STATE_KINE_ANGLE)
        sum += 3 * 4;
    if (mask & SerialComm::STATE_KINE_RATE)
        sum += 3 * 4;
    if (mask & SerialComm::STATE_KINE_ALTITUDE)
        sum += 4;
    if (mask & SerialComm::STATE_LOOP_COUNT)
        sum += 4;
    return sum;
}

void SerialComm::SendState(uint32_t timestamp_us, uint32_t mask, bool redirect_to_sd_card) const {
    // No need to build the message if we are not writing to the card
    if (redirect_to_sd_card && !sdcard::isOpen())
        return;
    if (!mask)
        mask = state_mask;
    // No need to publish empty state messages
    if (!mask)
        return;

    CobsPayloadGeneric payload;

    WriteProtocolHead(SerialComm::MessageType::State, mask, payload);

    if (mask & SerialComm::STATE_MICROS)
        payload.Append(timestamp_us);
    if (mask & SerialComm::STATE_STATUS)
        payload.Append(state->status);
    if (mask & SerialComm::STATE_V0)
        payload.Append(state->V0_raw);
    if (mask & SerialComm::STATE_I0)
        payload.Append(state->I0_raw);
    if (mask & SerialComm::STATE_I1)
        payload.Append(state->I1_raw);
    if (mask & SerialComm::STATE_ACCEL)
        payload.Append(state->accel);
    if (mask & SerialComm::STATE_GYRO)
        payload.Append(state->gyro);
    if (mask & SerialComm::STATE_MAG)
        payload.Append(state->mag);
    if (mask & SerialComm::STATE_TEMPERATURE)
        payload.Append(state->temperature);
    if (mask & SerialComm::STATE_PRESSURE)
        payload.Append(state->pressure);
    if (mask & SerialComm::STATE_RX_PPM) {
        for (int i = 0; i < 6; ++i)
            payload.Append(ppm[i]);
    }
    if (mask & SerialComm::STATE_AUX_CHAN_MASK)
        payload.Append(state->command_AUX_mask);
    if (mask & SerialComm::STATE_COMMANDS)
        payload.Append(state->command_throttle, state->command_pitch, state->command_roll, state->command_yaw);
    if (mask & SerialComm::STATE_F_AND_T)
        payload.Append(state->Fz, state->Tx, state->Ty, state->Tz);
    if (mask & SerialComm::STATE_PID_FZ_MASTER)
        WritePIDData(payload, control->thrust_pid.master());
    if (mask & SerialComm::STATE_PID_TX_MASTER)
        WritePIDData(payload, control->pitch_pid.master());
    if (mask & SerialComm::STATE_PID_TY_MASTER)
        WritePIDData(payload, control->roll_pid.master());
    if (mask & SerialComm::STATE_PID_TZ_MASTER)
        WritePIDData(payload, control->yaw_pid.master());
    if (mask & SerialComm::STATE_PID_FZ_SLAVE)
        WritePIDData(payload, control->thrust_pid.slave());
    if (mask & SerialComm::STATE_PID_TX_SLAVE)
        WritePIDData(payload, control->pitch_pid.slave());
    if (mask & SerialComm::STATE_PID_TY_SLAVE)
        WritePIDData(payload, control->roll_pid.slave());
    if (mask & SerialComm::STATE_PID_TZ_SLAVE)
        WritePIDData(payload, control->yaw_pid.slave());
    if (mask & SerialComm::STATE_MOTOR_OUT)
        payload.Append(state->MotorOut);
    if (mask & SerialComm::STATE_KINE_ANGLE)
        payload.Append(state->kinematicsAngle);
    if (mask & SerialComm::STATE_KINE_RATE)
        payload.Append(state->kinematicsRate);
    if (mask & SerialComm::STATE_KINE_ALTITUDE)
        payload.Append(state->kinematicsAltitude);
    if (mask & SerialComm::STATE_LOOP_COUNT)
        payload.Append(state->loopCount);
    WriteToOutput(payload, redirect_to_sd_card);
}

void SerialComm::SendResponse(uint32_t mask, uint32_t response) const {
    CobsPayload<12> payload;
    WriteProtocolHead(MessageType::Response, mask, payload);
    payload.Append(response);
    WriteToOutput(payload);
}

uint16_t SerialComm::GetSendStateDelay() const {
    return send_state_delay;
}

uint16_t SerialComm::GetSdCardStateDelay() const {
    return sd_card_state_delay;
}

void SerialComm::SetStateMsg(uint32_t values) {
    state_mask = values;
}

void SerialComm::AddToStateMsg(uint32_t values) {
    state_mask |= values;
}

void SerialComm::RemoveFromStateMsg(uint32_t values) {
    state_mask &= ~values;
}
