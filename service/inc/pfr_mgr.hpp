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

#pragma once

#include "pfr.hpp"

#include <boost/asio.hpp>
#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <string>

namespace pfr
{

static constexpr const char* versionPurposeBMC =
    "xyz.openbmc_project.Software.Version.VersionPurpose.BMC";
static constexpr const char* versionPurposeHost =
    "xyz.openbmc_project.Software.Version.VersionPurpose.Host";
static constexpr const char* versionPurposeOther =
    "xyz.openbmc_project.Software.Version.VersionPurpose.Other";

static constexpr const char* versionStr = "Version";
static constexpr const char* ufmProvisionedStr = "UfmProvisioned";
static constexpr const char* ufmLockedStr = "UfmLocked";
static constexpr const char* ufmSupportStr = "UfmSupport";

class PfrVersion
{
  public:
    PfrVersion(sdbusplus::asio::object_server& srv_,
               std::shared_ptr<sdbusplus::asio::connection>& conn_,
               const std::string& path_, const ImageType& imgType_,
               const std::string& purpose_);
    ~PfrVersion() = default;

    std::shared_ptr<sdbusplus::asio::connection> conn;

    void updateVersion();

  private:
    sdbusplus::asio::object_server& server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> versionIface;
    bool internalSet = false;

    std::string path;
    std::string version;
    std::string purpose;
    ImageType imgType;
};

class PfrConfig
{
  public:
    PfrConfig(sdbusplus::asio::object_server& srv_,
              std::shared_ptr<sdbusplus::asio::connection>& conn_);
    ~PfrConfig() = default;

    std::shared_ptr<sdbusplus::asio::connection> conn;

    void updateProvisioningStatus();

    bool getPfrProvisioned() const
    {
        return ufmProvisioned;
    }

  private:
    sdbusplus::asio::object_server& server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> pfrCfgIface;
    bool internalSet = false;

    bool ufmProvisioned;
    bool ufmLocked;
    bool ufmSupport;
};

// Firmware resiliency major map.
// {<CPLD association>, {<Redfish MessageID>, <Error reason> })
static const boost::container::flat_map<uint8_t,
                                        std::pair<std::string, std::string>>
    majorErrorCodeMapRev2 = {
        {0x03, {"FirmwareResiliencyError", "Firmware update failed"}}};

// postcode (platform state) map.
static const boost::container::flat_map<uint8_t, std::string> postcodeMap = {
    {0x00, "Postcode unavailable"},
    {0x01, "CPLD Nios II processor waiting to start"},
    {0x02, "CPLD Nios II processor started"},
    {0x03, "Enter T-1"},
    {0x04, "T-1 reserved 4"},
    {0x05, "T-1 Reserved 5"},
    {0x06, "BMC flash authentication"},
    {0x07, "PCH/CPU flash authentication"},
    {0x08, "Lockdown due to authentication failures"},
    {0x09, "Enter T0"},
    {0x0A, "T0 BMC booted"},
    {0x0B, "T0 ME booted"},
    {0x0C, "T0 Modular booted"},
    {0x0C, "T0 BIOS booted"},
    {0x0E, "T0 boot complete"},
    {0x0F, "T0 Reserved 0xF"},
    {0x10, "PCH/CPU firmware update"},
    {0x11, "BMC firmware update"},
    {0x12, "CPLD update (in CPLD Active Image)"},
    {0x13, "CPLD update (in CPLD ROM)"},
    {0x14, "PCH/CPU firmware volume update"},
    {0x15, "CPLD Nios II processor waiting to start"},
    {0x16, "Reserved 0x16"},
    {0x40, "T-1 firmware recovery due to authentication failure"},
    {0x41, "T-1 forced active firmware recovery"},
    {0x42, "WDT timeout recovery"},
    {0x43, "CPLD recovery (in CPLD ROM)"},
    {0x44, "Lockdown due to PIT L1"},
    {0x45, "PIT L2 firmware sealed"},
    {0x46, "Lockdown due to PIT L2 PCH/CPU firmware hash mismatch"},
    {0x47, "Lockdown due to PIT L2 BMC firmware hash mismatch"},
    {0x48, "Reserved 0x48"}};

class PfrPostcode
{
  public:
    PfrPostcode(sdbusplus::asio::object_server& srv_,
                std::shared_ptr<sdbusplus::asio::connection>& conn_);
    ~PfrPostcode() = default;

    std::shared_ptr<sdbusplus::asio::connection> conn;

    void updatePostcode();

  private:
    sdbusplus::asio::object_server& server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> pfrPostcodeIface;
    bool internalSet = false;
    uint8_t postcode;
};

} // namespace pfr
