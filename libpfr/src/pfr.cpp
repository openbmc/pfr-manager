/*
// Copyright (c) 2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "pfr.hpp"

#include "file.hpp"
#include "spiDev.hpp"

#include <gpiod.hpp>

#include <iomanip>
#include <iostream>
#include <sstream>

namespace pfr
{

// CPLD mailbox registers
static constexpr uint8_t pfrROTId = 0x00;
static constexpr uint8_t cpldROTVersion = 0x01;
static constexpr uint8_t cpldROTSvn = 0x02;
static constexpr uint8_t platformState = 0x03;
static constexpr uint8_t recoveryCount = 0x04;
static constexpr uint8_t lastRecoveryReason = 0x05;
static constexpr uint8_t panicEventCount = 0x06;
static constexpr uint8_t panicEventReason = 0x07;
static constexpr uint8_t majorErrorCode = 0x08;
static constexpr uint8_t minorErrorCode = 0x09;
static constexpr uint8_t provisioningStatus = 0x0A;
static constexpr uint8_t bmcBootCheckpointRev1 = 0x0F;
static constexpr uint8_t bmcBootCheckpoint = 0x60;
static constexpr uint8_t pchActiveMajorVersion = 0x15;
static constexpr uint8_t pchActiveMinorVersion = 0x16;
static constexpr uint8_t pchRecoveryMajorVersion = 0x1B;
static constexpr uint8_t pchRecoveryMinorVersion = 0x1C;
static constexpr uint8_t CPLDHashRegStart = 0x20;
static constexpr uint8_t pfrRoTValue = 0xDE;

static constexpr uint8_t ufmLockedMask = (0x1 << 0x04);
static constexpr uint8_t ufmProvisionedMask = (0x1 << 0x05);

// PFR MTD devices
static constexpr const char* bmcActiveImgPfmMTDDev = "/dev/mtd/pfm";
static constexpr const char* bmcRecoveryImgMTDDev = "/dev/mtd/rc-image";

// PFM offset in full image
static constexpr const uint32_t pfmBaseOffsetInImage = 0x400;

// OFFSET values in PFM
static constexpr const uint32_t verOffsetInPFM = 0x406;
static constexpr const uint32_t buildNumOffsetInPFM = 0x40C;
static constexpr const uint32_t buildHashOffsetInPFM = 0x40D;

static const std::array<std::string, 8> mainCPLDGpioLines = {
    "MAIN_PLD_MAJOR_REV_BIT3", "MAIN_PLD_MAJOR_REV_BIT2",
    "MAIN_PLD_MAJOR_REV_BIT1", "MAIN_PLD_MAJOR_REV_BIT0",
    "MAIN_PLD_MINOR_REV_BIT3", "MAIN_PLD_MINOR_REV_BIT2",
    "MAIN_PLD_MINOR_REV_BIT1", "MAIN_PLD_MINOR_REV_BIT0"};

static const std::array<std::string, 8> pldGpioLines = {
    "SGPIO_PLD_MAJOR_REV_BIT3", "SGPIO_PLD_MAJOR_REV_BIT2",
    "SGPIO_PLD_MAJOR_REV_BIT1", "SGPIO_PLD_MAJOR_REV_BIT0",
    "SGPIO_PLD_MINOR_REV_BIT3", "SGPIO_PLD_MINOR_REV_BIT2",
    "SGPIO_PLD_MINOR_REV_BIT1", "SGPIO_PLD_MINOR_REV_BIT0"};

std::string toHexString(const uint8_t val)
{
    std::stringstream stream;
    stream << std::setfill('0') << std::setw(2) << std::hex
           << static_cast<int>(val);
    return stream.str();
}

static std::string readCPLDHash()
{
    std::stringstream hashStrStream;
    static constexpr uint8_t hashLength = 32;
    std::array<uint8_t, hashLength> hashValue = {0};
    try
    {
        I2CFile cpldDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);
        if (cpldDev.i2cReadBlockData(CPLDHashRegStart, hashLength,
                                     hashValue.data()))
        {
            for (const auto& i : hashValue)
            {
                hashStrStream << std::setfill('0') << std::setw(2) << std::hex
                              << static_cast<int>(i);
            }
        }
        else
        {
            // read failed
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Failed to read CPLD Hash string");
            return "";
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in readCPLDHash.",
            phosphor::logging::entry("MSG=%s", e.what()));
        return "";
    }
    return hashStrStream.str();
}

static std::string readVersionFromCPLD(const uint8_t majorReg,
                                       const uint8_t minorReg)
{
    try
    {
        I2CFile cpldDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);
        uint8_t majorVer = cpldDev.i2cReadByteData(majorReg);
        uint8_t minorVer = cpldDev.i2cReadByteData(minorReg);
        // Major and Minor versions should be binary encoded strings.
        std::string version =
            std::to_string(majorVer) + "." + std::to_string(minorVer);
        return version;
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in readVersionFromCPLD.",
            phosphor::logging::entry("MSG=%s", e.what()));
        return "";
    }
}

static std::string readBMCVersionFromSPI(const ImageType& imgType)
{
    std::string mtdDev;
    uint32_t verOffset = verOffsetInPFM;
    uint32_t bldNumOffset = buildNumOffsetInPFM;
    uint32_t bldHashOffset = buildHashOffsetInPFM;

    if (imgType == ImageType::bmcActive)
    {
        // For Active image, PFM is emulated as separate MTD device.
        mtdDev = bmcActiveImgPfmMTDDev;
    }
    else if (imgType == ImageType::bmcRecovery)
    {
        // For Recovery image, PFM is part of compressed Image
        // at offset 0x400.
        mtdDev = bmcRecoveryImgMTDDev;
        verOffset += pfmBaseOffsetInImage;
        bldNumOffset += pfmBaseOffsetInImage;
        bldHashOffset += pfmBaseOffsetInImage;
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid image type passed to readBMCVersionFromSPI.");
        return "";
    }

    uint8_t buildNo = 0;
    std::array<uint8_t, 2> ver;
    std::array<uint8_t, 3> buildHash;

    try
    {
        SPIDev spiDev(mtdDev);
        spiDev.spiReadData(verOffset, ver.size(),
                           reinterpret_cast<void*>(ver.data()));
        spiDev.spiReadData(bldNumOffset, sizeof(buildNo),
                           reinterpret_cast<void*>(&buildNo));
        spiDev.spiReadData(bldHashOffset, buildHash.size(),
                           reinterpret_cast<void*>(buildHash.data()));
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in readBMCVersionFromSPI.",
            phosphor::logging::entry("MSG=%s", e.what()));
        return "";
    }

    // Version format: <major>.<minor>-<build bum>-g<build hash>
    // Example: 0.11-7-g1e5c2d
    // Major, minor and build numberare BCD encoded.
    std::string version =
        std::to_string(ver[0]) + "." + std::to_string(ver[1]) + "-" +
        std::to_string(buildNo) + "-g" + toHexString(buildHash[0]) +
        toHexString(buildHash[1]) + toHexString(buildHash[2]);
    return version;
}

static bool getGPIOInput(const std::string& name, gpiod::line& gpioLine,
                         uint8_t* value)
{
    // Find the GPIO line
    gpioLine = gpiod::find_line(name);
    if (!gpioLine)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to find the GPIO line: ",
            phosphor::logging::entry("MSG=%s", name.c_str()));
        return false;
    }
    try
    {
        gpioLine.request({__FUNCTION__, gpiod::line_request::DIRECTION_INPUT});
    }
    catch (std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to request the GPIO line",
            phosphor::logging::entry("MSG=%s", e.what()));
        gpioLine.release();
        return false;
    }
    try
    {
        *value = gpioLine.get_value();
    }
    catch (std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to get the value of GPIO line",
            phosphor::logging::entry("MSG=%s", e.what()));
        gpioLine.release();
        return false;
    }
    gpioLine.release();
    return true;
}

static std::string readCPLDVersion()
{
    // CPLD SGPIO lines
    gpiod::line mainCPLDLine;
    gpiod::line pldLine;
    // read main pld and pld version
    uint8_t mainCPLDVer = 0;
    uint8_t pldVer = 0;
    // main CPLD
    for (const auto& gLine : mainCPLDGpioLines)
    {
        uint8_t value = 0;
        if (getGPIOInput(gLine, mainCPLDLine, &value))
        {
            mainCPLDVer <<= 1;
            mainCPLDVer = mainCPLDVer | value;
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Failed to read GPIO line: ",
                phosphor::logging::entry("MSG=%s", gLine.c_str()));
            mainCPLDVer = 0;
            break;
        }
    }

    // pld lines
    for (const auto& gLine : pldGpioLines)
    {
        uint8_t value = 0;
        if (getGPIOInput(gLine, pldLine, &value))
        {
            pldVer <<= 1;
            pldVer = pldVer | value;
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Failed to read GPIO line: ",
                phosphor::logging::entry("MSG=%s", gLine.c_str()));
            pldVer = 0;
            break;
        }
    }

    std::string svnRoTHash = "";

    // check if reg 0x00 read 0xde
    uint8_t cpldRoTValue = 0;
    try
    {
        I2CFile cpldDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);
        cpldRoTValue = cpldDev.i2cReadByteData(pfrROTId);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in readCPLDVersion.",
            phosphor::logging::entry("MSG=%s", e.what()));
    }

    if (cpldRoTValue == pfrRoTValue)
    {
        // read SVN and RoT version
        std::string svnRoTver = readVersionFromCPLD(cpldROTVersion, cpldROTSvn);

        // read CPLD hash
        std::string cpldHash = readCPLDHash();
        svnRoTHash = "-" + svnRoTver + "-" + cpldHash;
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "PFR-CPLD not present.");
    }

    // CPLD version format:
    // When PFR CPLD is present
    // <MainPLDMajorMinor.PLDMajorMinor>-<SVN.RoT>-<CPLD-Hash>
    // Example: 2.7-1.1-<Hash string>

    // When Non-PFR CPLD is present -> <MainPLDMajorMinor.PLDMajorMinor>
    // Example: 2.7

    std::string version =
        std::to_string(mainCPLDVer) + "." + std::to_string(pldVer) + svnRoTHash;
    return version;
}

std::string getFirmwareVersion(const ImageType& imgType)
{
    switch (imgType)
    {
        case (ImageType::cpldActive):
        {
            return readCPLDVersion();
        }
        case (ImageType::cpldRecovery):
        {
            // TO-DO: Need to update once CPLD supported Firmware is available
            return readVersionFromCPLD(cpldROTVersion, cpldROTSvn);
        }
        case (ImageType::biosActive):
        {
            return readVersionFromCPLD(pchActiveMajorVersion,
                                       pchActiveMinorVersion);
        }
        case (ImageType::biosRecovery):
        {
            return readVersionFromCPLD(pchRecoveryMajorVersion,
                                       pchRecoveryMinorVersion);
        }
        case (ImageType::bmcActive):
        case (ImageType::bmcRecovery):
        {
            return readBMCVersionFromSPI(imgType);
        }
        default:
            // Invalid image Type.
            return "";
    }
}

int getProvisioningStatus(bool& ufmLocked, bool& ufmProvisioned,
                          bool& ufmSupport)
{
    try
    {
        I2CFile cpldDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);
        uint8_t provStatus = cpldDev.i2cReadByteData(provisioningStatus);
        uint8_t pfrRoT = cpldDev.i2cReadByteData(pfrROTId);
        ufmLocked = (provStatus & ufmLockedMask);
        ufmProvisioned = (provStatus & ufmProvisionedMask);
        ufmSupport = (pfrRoT & pfrRoTValue);
        return 0;
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in getProvisioningStatus.",
            phosphor::logging::entry("MSG=%s", e.what()));
        return -1;
    }
}

int readCpldReg(const ActionType& action, uint8_t& value)
{
    uint8_t cpldReg;

    switch (action)
    {
        case (ActionType::recoveryCount):
            cpldReg = recoveryCount;
            break;
        case (ActionType::recoveryReason):
            cpldReg = lastRecoveryReason;
            break;
        case (ActionType::panicCount):
            cpldReg = panicEventCount;
            break;
        case (ActionType::panicReason):
            cpldReg = panicEventReason;
            break;
        case (ActionType::majorError):
            cpldReg = majorErrorCode;
            break;
        case (ActionType::minorError):
            cpldReg = minorErrorCode;
            break;

        default:
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Invalid CPLD read action.");
            return -1;
    }

    try
    {
        I2CFile cpldDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);
        value = cpldDev.i2cReadByteData(cpldReg);
        return 0;
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in readCpldReg.",
            phosphor::logging::entry("MSG=%s", e.what()));
        return -1;
    }
}

int setBMCBootCheckpoint(const uint8_t checkPoint)
{
    uint8_t bmcBootCheckpointReg = bmcBootCheckpoint;

    // check if reg 0x01(RoTRev) is 1 or 2.
    // checkpoint register changes for RoT Rev 1 and 2
    uint8_t cpldRoTRev = 0;
    try
    {
        I2CFile cpldDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);
        cpldRoTRev = cpldDev.i2cReadByteData(cpldROTVersion);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in reading RoT rev.",
            phosphor::logging::entry("MSG=%s", e.what()));
        return -1;
    }

    // latest PFR has different check point register
    if (cpldRoTRev <= 1)
    {
        bmcBootCheckpointReg = bmcBootCheckpointRev1;
    }

    try
    {
        I2CFile cpldDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);
        cpldDev.i2cWriteByteData(bmcBootCheckpointReg, checkPoint);
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "Successfully set the PFR CPLD checkpoint 9.");
        return 0;
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Exception caught in setBMCBootCheckout.",
            phosphor::logging::entry("MSG=%s", e.what()));
        return -1;
    }
}

} // namespace pfr
