/*
 * This file is part of the ZombieVerter project.
 *
 * Copyright (C) 2020 Johannes Huebner <dev@johanneshuebner.com>
 *               2021-2022 Damien Maguire <info@evbmw.com>
 * Yes I'm really writing software now........run.....run away.......
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "NissanPDM.h"
#include "my_fp.h"
#include "my_math.h"
#include "stm32_can.h"
#include "params.h"
#include "utils.h"

static uint16_t Vbatt=0;
static uint16_t VbattSP=0;
static uint8_t mprun10=0;
static uint8_t mprun100=0;
static uint8_t OBCpwrSP=0;
static uint8_t OBCpwr=0;
static bool PPStat = false;
static uint8_t OBCVoltStat=0;
static uint8_t PlugStat=0;
static uint16_t calcBMSpwr=0;
static bool BMSspoof = true;
static uint8_t OBCAvailPwr = 0;
static uint8_t OBCActPwr = 0;

/*Info on running Leaf Gen 2,3 PDM
IDs required :
0x1D4 VCM (10ms)
0x1DB LBC (10ms)
0x1DC LBC (10ms)
0x1F2 VCM (10ms)
0x50B VCM (100ms)
0x55B LBC (100ms)
0x59E LBC (500ms)
0x5BC LBC (100ms)
PDM sends:
0x390 (100ms)
0x393 (100ms)
0x679 on evse plug insert

For QC:

From PDM to QC

PDM EV CAN --------to-------QC CAN
0x3b9                       0x100
0x3bb                       0x101
0x3bc                       0x102
0x3be                       0x200
0x4ba                       0x110
0x4bb                       0x201
0x4bc                       0x700
0x4c0                       0x202

From QC to PDM

QC CAN----------------------PDM EV CAN
0x108                       0x3c8
0x109                       0x3c9
0x208                       0x3cd
0x209                       0x4be


*/

void NissanPDM::SetCanInterface(CanHardware* c)
{
    can = c;
    can->RegisterUserMessage(0x679);//Leaf obc msg
    can->RegisterUserMessage(0x390);//Leaf obc msg
}

void NissanPDM::DecodeCAN(int id, uint32_t data[2])
{
    uint8_t* bytes = (uint8_t*)data;// arrgghhh this converts the two 32bit array into bytes. See comments are useful:)

    if (id == 0x390)// THIS MSG FROM PDM
    {
        OBCVoltStat = (bytes[3] >> 3) & 0x03;

        if(OBCVoltStat == 0x1)
        {
            Param::SetInt(Param::AC_Volts,110);
        }
        else if(OBCVoltStat == 0x2)
        {
            Param::SetInt(Param::AC_Volts,230);
        }
        else
        {
            Param::SetInt(Param::AC_Volts,0);
        }

        OBCActPwr = bytes[1]; //Power in 0.1kW
        OBCAvailPwr = bytes[6]; //Power in 0.1kW

        PlugStat = bytes[5] & 0x0F;
        if(PlugStat == 0x08) PPStat = true; //plug inserted
        if(PlugStat == 0x00) PPStat = false; //plug not inserted

        Param::SetInt(Param::PlugDet,PPStat);
    }
}

bool NissanPDM::ControlCharge(bool RunCh, bool ACReq) //Modeled off of Outlander Charger
{
    bool dummy=RunCh;
    dummy=dummy;

    int chgmode = Param::GetInt(Param::interface);
    switch(chgmode)
    {
    case Unused:
        if(PPStat && ACReq)
        {
            return true;
        }
        else
        {
            return false;
        }

        break;

    case i3LIM:
        if(RunCh && ACReq)//we have a startup request to AC charge from a charge interface
        {
            return true;
        }
        else
        {
            return false;
        }
        break;

    case CPC:
        if(RunCh && ACReq)//we have a startup request to AC charge from a charge interface
        {
            return true;
        }
        else
        {
            return false;
        }
        break;

    case Foccci:
        if(RunCh && ACReq)//we have a startup request to AC charge from a charge interface
        {
            return true;
        }
        else
        {
            return false;
        }
        break;

    case Chademo:
        if (RunCh && ACReq)
        {
            return true;
        }
        else
        {
            return false;
        }

        break;

    }
    return false;
}


void NissanPDM::Task10Ms()
{
    int opmode = Param::GetInt(Param::opmode);
    if (Param::GetInt(Param::BMS_Mode) == 4)
    {
        BMSspoof = false;
    }
    else
    {
        BMSspoof = true;
    }

    uint8_t bytes[8];
    mprun10 = (mprun10 + 1) % 4; // mprun10 cycles between 0-1-2-3-0-1...

    if (opmode == MOD_CHARGE) //ONLY send when charging else sent by leafinv.cpp
    {

        /////////////////////////////////////////////////////////////////////////////////////////////////
        // CAN Messaage 0x11A

        // Data taken from a gen1 inFrame where the car is starting to
        // move at about 10% throttle: 4E400055 0000017D

        // All possible gen1 values: 00 01 0D 11 1D 2D 2E 3D 3E 4D 4E
        // MSB nibble: Selected gear (gen1/LeafLogs)
        //   0: some kind of non-gear before driving
        //      0: Park in Gen 2. byte 0 = 0x01 when in park and charging
        //   1: some kind of non-gear after driving
        //   2: R
        //   3: N
        //   4: D
        // LSB nibble: ? (LeafLogs)
        //   0: sometimes at startup, not always; never when the
        //      inverted is powered on (0.06%)
        //   1: this is the usual value (55% of the time in LeafLogs)
        //   D: seems to occur for ~90ms when changing gears (0.2%)
        //   E: this also is a usual value, but never occurs with the
        //      non-gears 0 and 1 (44% of the time in LeafLogs)


        //byte 0 determines motor rotation direction
        bytes[0] = 0x01;//Car in park when charging
        // 0x40 when car is ON, 0x80 when OFF, 0x50 when ECO. Car must be off when charing 0x80
        bytes[1] = 0x80;
        // Usually 0x00, sometimes 0x80 (LeafLogs), 0x04 seen by canmsgs
        bytes[2] = 0x00;

        // Weird value at D3:4 that goes along with the counter
        // NOTE: Not actually needed, you can just send constant AA C0
        const static uint8_t weird_d34_values[4][2] =
        {
            {0xaa, 0xc0},
            {0x55, 0x00},
            {0x55, 0x40},
            {0xaa, 0x80},
        };

        bytes[3] = weird_d34_values[mprun10][0];//0xAA;
        bytes[4] = weird_d34_values[mprun10][1];//0xC0;
        bytes[5] = 0x00;  // Always 0x00 (LeafLogs, canmsgs)
        bytes[6] = mprun10;     // A 2-bit counter
        nissan_crc(bytes, 0x85);

        can->Send(0x11A, (uint32_t*)bytes, 8);


        /////////////////////////////////////////////////////////////////////////////////////////////////
        // CAN Message 0x1D4: Target Motor Torque

        // Data taken from a gen1 inFrame where the car is starting to
        // move at about 10% throttle: F70700E0C74430D4

        // Usually F7, but can have values between 9A...F7 (gen1)
        bytes[0] = 0xF7;

        // Usually 07, but can have values between 07...70 (gen1)
        bytes[1] = 0x07;

        bytes[2] = 0x00;
        bytes[3] = 0x00;

        // MSB nibble: Runs through the sequence 0, 4, 8, C
        // LSB nibble: Precharge report (precedes actual precharge
        //             control)
        //   0: Discharging (5%)
        //   2: Precharge not started (1.4%)
        //   3: Precharging (0.4%)
        //   5: Starting discharge (3x10ms) (2.0%)
        //   7: Precharged (93%)
        bytes[4] = 0x07 | (mprun10 << 6);
        //bytes[4] = 0x02 | (mprun10 << 6);
        //Bit 2 is HV status. 0x00 No HV, 0x01 HV On.

        // MSB nibble:
        //   0: 35-40ms at startup when gear is 0, then at shutdown 40ms
        //      after the car has been shut off (6% total)
        //   4: Otherwise (94%)
        // LSB nibble:
        //   0: ~100ms when changing gear, along with 11A D0 b3:0 value
        //      D (0.3%)
        //   2: Reverse gear related (13%)
        //   4: Forward gear related (21%)
        //   6: Occurs always when gear 11A D0 is 01 or 11 (66%)
        //outFrame.data.bytes[5] = 0x44;
        //outFrame.data.bytes[5] = 0x46;

        // 2016 drive cycle: 06, 46, precharge, 44, drive, 46, discharge, 06
        // 0x46 requires ~25 torque to start
        //outFrame.data.bytes[5] = 0x46;
        // 0x44 requires ~8 torque to start
        bytes[5] = 0x44;
        bytes[6] = 0xE0;   //charging mode

        // Extra CRC
        nissan_crc(bytes, 0x85);

        can->Send(0x1D4, (uint32_t*)bytes, 8);//send on can1
    }


    if(BMSspoof)
    {
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // CAN Message 0x1DB

        //We need to send 0x1db here with voltage measured by inverter
        //Zero seems to work also on my gen1
        ////////////////////////////////////////////////////////////////
        //Byte 1 bits 8-10 LB Failsafe Status
        //0x00 Normal start req. seems to stay on this value most of the time
        //0x01 Normal stop req
        //0x02 Charge stop req
        //0x03 Charge and normal stop req. Other values call for a caution lamp which we don't need
        //bits 11-12 LB relay cut req
        //0x00 no req
        //0x01,0x02,0x03 main relay off req
        s16fp TMP_battI = (Param::Get(Param::idc))*2;
        s16fp TMP_battV = (Param::Get(Param::udc))*4;
        bytes[0] = TMP_battI >> 8;     //MSB current. 11 bit signed MSBit first
        bytes[1] = TMP_battI & 0xE0;  //LSB current bits 7-5. Dont need to mess with bits 0-4 for now as 0 works.
        bytes[2] = TMP_battV >> 8;
        bytes[3] = ((TMP_battV & 0xC0) | (0x2b)); //0x2b should give no cut req, main rly on permission,normal p limit.
        bytes[4] = 0x40;  //SOC for dash in Leaf. fixed val.
        bytes[5] = 0x00;
        bytes[6] = mprun10;

        // Extra CRC in byte 7
        nissan_crc(bytes, 0x85);

        can->Send(0x1DB, (uint32_t*)bytes, 8);
    }


    /////////////////////////////////////////////////////////////////////////////////////////////////
    // CAN Message 0x50B

    // Statistics from 2016 capture:
    //     10 00000000000000
    //     21 000002c0000000
    //    122 000000c0000000
    //    513 000006c0000000

    // Let's just send the most common one all the time
    // FIXME: This is a very sloppy implementation. Thanks. I try:) Dala: This message is not 10ms, it is 100ms
    bytes[0] = 0x00;
    bytes[1] = 0x00;
    bytes[2] = 0x06;
    bytes[3] = 0xc0;
    bytes[4] = 0x00;
    bytes[5] = 0x00;
    bytes[6] = 0x00;

    //possible problem here as 0x50B is DLC 7....
    can->Send(0x50B, (uint32_t*)bytes, 7);

    if(BMSspoof)
    {
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // CAN Message 0x1DC:

        // 0x1dc from lbc. Contains chg power lims and disch power lims.
        // Disch power lim in byte 0 and byte 1 bits 6-7. Just set to max for now.
        // Max charging power in bits 13-20. 10 bit unsigned scale 0.25.Byte 1 limit in kw.
        bytes[0]=0x6E;
        bytes[1]=0x0A;
        bytes[2]=0x05;
        bytes[3]=0xD5;
        bytes[4]=0x00;//may not need pairing code crap here...and we don't:)
        bytes[5]=0x00;
        bytes[6]=mprun10;
        // Extra CRC in byte 7
        nissan_crc(bytes, 0x85);

        can->Send(0x1DC, (uint32_t*)bytes, 8);
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////
    // CAN Message 0x1F2: Charge Power and DC/DC Converter Control

    // convert power setpoint to PDM format:
    //    0x70 = 3 amps ish
    //    0x6a = 1.4A
    //    0x66 = 0.5A
    //    0x65 = 0.3A
    //    0x64 = no chg
    //    so 0x64=100. 0xA0=160. so 60 decimal steps. 1 step=100W???
    /////////////////////////////////////////////////////////////////////////////////////////////////////


    // get actual voltage and voltage setpoints
    Vbatt = Param::GetInt(Param::udc);
    VbattSP = Param::GetInt(Param::Voltspnt);
    calcBMSpwr=(Vbatt * Param::GetInt(Param::BMS_ChargeLim));//BMS charge current limit but needs to be power for most AC charger types.


    OBCpwrSP = (MIN(Param::GetInt(Param::Pwrspnt),calcBMSpwr) / 100) + 0x64;

    if (opmode == MOD_CHARGE && Param::GetInt(Param::Chgctrl) == ChargeControl::Enable)
    {
        // clamp min and max values
        if (OBCpwrSP > 0xA0)
            OBCpwrSP = 0xA0;
        else if (OBCpwrSP < 0x64)
            OBCpwrSP = 0x64;

        // if measured vbatt is less than setpoint got to max power from web ui
        if (Vbatt < VbattSP)
            OBCpwr = OBCpwrSP;

        // decrement charger power if volt setpoint is reached
        if (Vbatt >= VbattSP)
            OBCpwr--;
    }
    else
    {
        // set power to 0 if charge control is set to off or not in charge mode
        OBCpwr = 0x64;
    }


    // Commanded chg power in byte 1 and byte 0 bits 0-1. 10 bit number.
    // byte 1=0x64 and byte 0=0x00 at 0 power.
    // 0x00 chg 0ff dcdc on.
    bytes[0] = 0x30;  // msg is muxed but pdm doesn't seem to care.
    bytes[1] = OBCpwr;
    bytes[2] = 0x20;//0x20=Normal Charge
    bytes[3] = 0xAC;
    bytes[4] = 0x00;
    bytes[5] = 0x3C;
    bytes[6] = mprun10;
    bytes[7] = 0x8F;  //may not need checksum here?

    can->Send(0x1F2, (uint32_t*)bytes, 8);
}

void NissanPDM::Task100Ms()
{


    // MSGS for charging with pdm
    uint8_t bytes[8];
    mprun100 = (mprun100 + 1) % 4; // mprun100 cycles between 0-1-2-3-0-1...

    if(BMSspoof)
    {
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // CAN Message 0x55B:

        bytes[0] = 0xA4;
        bytes[1] = 0x40;
        bytes[2] = 0xAA;
        bytes[3] = 0x00;
        bytes[4] = 0xDF;
        bytes[5] = 0xC0;
        bytes[6] = ((0x1 << 4) | (mprun100));
        // Extra CRC in byte 7
        nissan_crc(bytes, 0x85);

        can->Send(0x55b, (uint32_t*)bytes, 8);

        /////////////////////////////////////////////////////////////////////////////////////////////////
        // CAN Message 0x59E:

        bytes[0] = 0x00;//Static msg works fine here
        bytes[1] = 0x00;//Batt capacity for chg and qc.
        bytes[2] = 0x0c;
        bytes[3] = 0x76;
        bytes[4] = 0x18;
        bytes[5] = 0x00;
        bytes[6] = 0x00;
        bytes[7] = 0x00;

        can->Send(0x59e, (uint32_t*)bytes, 8);

        /////////////////////////////////////////////////////////////////////////////////////////////////
        // CAN Message 0x5BC:

        // muxed msg with info for gids etc. Will try static for a test.
        bytes[0] = 0x3D;//Static msg works fine here
        bytes[1] = 0x80;
        bytes[2] = 0xF0;
        bytes[3] = 0x64;
        bytes[4] = 0xB0;
        bytes[5] = 0x01;
        bytes[6] = 0x00;
        bytes[7] = 0x32;

        can->Send(0x5bc, (uint32_t*)bytes, 8);
    }

    Param::SetInt(Param::PilotLim,float(OBCAvailPwr/2.25));

}


int8_t NissanPDM::fahrenheit_to_celsius(uint16_t fahrenheit)
{
    int16_t result = ((int16_t)fahrenheit - 32) * 5 / 9;
    if(result < -128)
        return -128;
    if(result > 127)
        return 127;
    return result;
}



void NissanPDM::nissan_crc(uint8_t *data, uint8_t polynomial)
{
    // We want to process 8 bytes with the 8th byte being zero
    data[7] = 0;
    uint8_t crc = 0;
    for(int b=0; b<8; b++)
    {
        for(int i=7; i>=0; i--)
        {
            uint8_t bit = ((data[b] &(1 << i)) > 0) ? 1 : 0;
            if(crc >= 0x80)
                crc = (uint8_t)(((crc << 1) + bit) ^ polynomial);
            else
                crc = (uint8_t)((crc << 1) + bit);
        }
    }
    data[7] = crc;
}
