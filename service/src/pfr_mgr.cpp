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

#include "pfr_mgr.hpp"

#include "file.hpp"

namespace pfr
{

inline void printVersion(const std::string& path, const std::string& version)
{
    lg2::info("VERSION INFO - {TYPE} - {VER}", "TYPE", path, "VER", version);
}
static constexpr uint8_t activeImage = 0;
static constexpr uint8_t recoveryImage = 1;

std::shared_ptr<sdbusplus::asio::dbus_interface> associationIface;
std::set<std::tuple<std::string, std::string, std::string>> associations;

static int i2cBusNumber = 4;
static int i2cSlaveAddress = 56;
bool i2cConfigLoaded = false;

using GetSubTreeType = std::vector<
    std::pair<std::string,
              std::vector<std::pair<std::string, std::vector<std::string>>>>>;

PfrVersion::PfrVersion(sdbusplus::asio::object_server& srv_,
                       std::shared_ptr<sdbusplus::asio::connection>& conn_,
                       const std::string& path_, const ImageType& imgType_,
                       const std::string& purpose_) :
    server(srv_),
    conn(conn_), path(path_), imgType(imgType_), purpose(purpose_)
{
    version = getFirmwareVersion(imgType);

    if (!(version == "0.0" || version.empty()))
    {
        printVersion(path, version);
    }

    std::string objPath = "/xyz/openbmc_project/software/" + path;
    versionIface =
        server.add_interface(objPath, "xyz.openbmc_project.Software.Version");

    if (versionIface != nullptr)
    {

        versionIface->register_property("Purpose", purpose);
        versionIface->register_property(
            versionStr, version,
            // Override set
            [this](const std::string& req, std::string& propertyValue) {
                if (internalSet)
                {
                    if (req != propertyValue)
                    {
                        version = req;
                        propertyValue = req;
                        return 1;
                    }
                }
                return 0;
            });

        versionIface->initialize();
    }

    std::string activation =
        "xyz.openbmc_project.Software.Activation.Activations.StandbySpare";

    if ((imgType == ImageType::bmcActive) ||
        (imgType == ImageType::biosActive) ||
        (imgType == ImageType::cpldActive) || (imgType == ImageType::afmActive))
    {
        // Running images so set Activations to "Active"
        activation =
            "xyz.openbmc_project.Software.Activation.Activations.Active";

        // For all Active images, functional endpoints must be added. This is
        // used in bmcweb & ipmi for fetching active component versions.

        // TODO: We have redundant active firmware version objects for BMC
        // and BIOS active images. BMC version is read from /etc/os-release
        // BIOS version is read from SMBIOS. Since it provides more
        // version information, Lets expose those as functional.
        // Down the line, Redundant inventory objects need to be addressed.
        if ((imgType == ImageType::cpldActive) ||
            (imgType == ImageType::afmActive))
        {
            associations.emplace("functional", "software_version", objPath);
        }
    }

    std::string reqActNone =
        "xyz.openbmc_project.Software.Activation.RequestedActivations.None";
    auto activationIface = server.add_interface(
        objPath, "xyz.openbmc_project.Software.Activation");

    if (activationIface != nullptr)
    {

        activationIface->register_property("Activation", activation);
        activationIface->register_property("RequestedActivation", reqActNone);

        activationIface->initialize();
    }

    // All the components exposed under PFR.Manager are updateable.
    // Lets add objPath endpoints to 'updatable' association
    associations.emplace("updateable", "software_version", objPath);
    associationIface->set_property("Associations", associations);
}

void PfrVersion::updateVersion()
{
    if (versionIface && versionIface->is_initialized())
    {
        std::string ver = getFirmwareVersion(imgType);
        printVersion(path, ver);
        internalSet = true;
        versionIface->set_property(versionStr, ver);
        internalSet = false;
    }
    return;
}

PfrConfig::PfrConfig(sdbusplus::asio::object_server& srv_,
                     std::shared_ptr<sdbusplus::asio::connection>& conn_) :
    server(srv_),
    conn(conn_)
{
    pfrCfgIface = server.add_interface("/xyz/openbmc_project/pfr",
                                       "xyz.openbmc_project.PFR.Attributes");

    ufmLocked = false;
    ufmProvisioned = false;
    ufmSupport = false;
    getProvisioningStatus(ufmLocked, ufmProvisioned, ufmSupport);

    pfrCfgIface->register_property(ufmProvisionedStr, ufmProvisioned,
                                   // Override set
                                   [this](const bool req, bool propertyValue) {
                                       if (internalSet)
                                       {
                                           if (req != propertyValue)
                                           {
                                               ufmProvisioned = req;
                                               propertyValue = req;
                                               return 1;
                                           }
                                       }
                                       return 0;
                                   });

    pfrCfgIface->register_property(ufmLockedStr, ufmLocked,
                                   // Override set
                                   [this](const bool req, bool propertyValue) {
                                       if (internalSet)
                                       {
                                           if (req != propertyValue)
                                           {
                                               ufmLocked = req;
                                               propertyValue = req;
                                               return 1;
                                           }
                                       }
                                       return 0;
                                   });

    pfrCfgIface->register_property(ufmSupportStr, ufmSupport,
                                   // Override set
                                   [this](const bool req, bool propertyValue) {
                                       if (internalSet)
                                       {
                                           if (req != propertyValue)
                                           {
                                               ufmSupport = req;
                                               propertyValue = req;
                                               return 1;
                                           }
                                       }
                                       return 0;
                                   });

    pfrCfgIface->initialize();

    /*BMCBusy period MailBox handling */
    pfrMBIface = server.add_interface("/xyz/openbmc_project/pfr",
                                      "xyz.openbmc_project.PFR.Mailbox");

    conn_->async_method_call(
        [conn_](const boost::system::error_code ec,
                const GetSubTreeType& resp) {
            if (ec || resp.size() != 1)
            {
                return;
            }
            if (resp[0].second.begin() == resp[0].second.end())
                return;
            const std::string& objPath = resp[0].first;
            const std::string& serviceName = resp[0].second.begin()->first;

            const std::string match = "Baseboard/PFR";
            if (boost::ends_with(objPath, match))
            {
                // PFR object found.. check for PFR support
                conn_->async_method_call(
                    [objPath, serviceName, conn_](
                        boost::system::error_code ec,
                        const std::vector<std::pair<
                            std::string, std::variant<std::string, uint64_t>>>&
                            propertiesList) {
                        if (ec)
                        {
                            phosphor::logging::log<
                                phosphor::logging::level::ERR>(
                                "Error to Get PFR properties.",
                                phosphor::logging::entry("MSG=%s",
                                                         ec.message().c_str()));
                            return;
                        }

                        const uint64_t* i2cBus = nullptr;
                        const uint64_t* address = nullptr;

                        for (const auto& [propName, propVariant] :
                             propertiesList)
                        {
                            if (propName == "Address")
                            {
                                address = std::get_if<uint64_t>(&propVariant);
                            }
                            else if (propName == "Bus")
                            {
                                i2cBus = std::get_if<uint64_t>(&propVariant);
                            }
                        }

                        if ((address == nullptr) || (i2cBus == nullptr))
                        {
                            phosphor::logging::log<
                                phosphor::logging::level::ERR>(
                                "Unable to read the pfr properties");
                            return;
                        }

                        i2cBusNumber = static_cast<int>(*i2cBus);
                        i2cSlaveAddress = static_cast<int>(*address);
                        i2cConfigLoaded = true;
                    },
                    serviceName, objPath, "org.freedesktop.DBus.Properties",
                    "GetAll", "xyz.openbmc_project.Configuration.PFR");
            }
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory/system", 0,
        std::array<const char*, 1>{"xyz.openbmc_project.Configuration.PFR"});

    pfrMBIface->register_method("InitiateBMCBusyPeriod", [this, i2cBusNumber,
                                                          i2cSlaveAddress](
                                                             bool setReset) {
        uint8_t bmcBusyReg = 0x63;
        uint8_t valHigh = 0x01;
        uint8_t mailBoxReply = 0;

        try
        {
            I2CFile mailDev(i2cBusNumber, i2cSlaveAddress, O_RDWR | O_CLOEXEC);

            mailBoxReply = mailDev.i2cReadByteData(bmcBusyReg);

            uint8_t readValue = mailBoxReply | valHigh;

            if (setReset == false)
            {
                readValue = 0x00;
            }

            mailDev.i2cWriteByteData(bmcBusyReg, readValue);

            phosphor::logging::log<phosphor::logging::level::INFO>(
                "Successfully set the PFR MailBox to BMCBusy.");
        }
        catch (const std::exception& e)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Exception caught in setting PFR Mailbox to BMCBusy.",
                phosphor::logging::entry("MSG=%s", e.what()));
            return false;
        }
        return true;
    });

    pfrMBIface->register_method(
        "ReadMBRegister",
        [i2cBusNumber, i2cSlaveAddress](uint32_t regAddr) -> uint8_t {
            // Read from PFR CPLD's mailbox register
            uint8_t mailBoxReply = 0;
            try
            {

                I2CFile mailReadDev(i2cBusNumber, i2cSlaveAddress,
                                    O_RDWR | O_CLOEXEC);

                mailBoxReply = mailReadDev.i2cReadByteData(regAddr);
            }
            catch (const std::exception& e)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Exception caught in mailbox reading.",
                    phosphor::logging::entry("MSG=%s", e.what()));
                return -1;
            }
            return mailBoxReply;
        });
    pfrMBIface->initialize();

    associationIface =
        server.add_interface("/xyz/openbmc_project/software",
                             "xyz.openbmc_project.Association.Definitions");
    associationIface->register_property("Associations", associations);
    associationIface->initialize();
}

void PfrConfig::updateProvisioningStatus()
{
    if (pfrCfgIface && pfrCfgIface->is_initialized())
    {
        bool lockVal = false;
        bool provVal = false;
        bool supportVal = false;
        getProvisioningStatus(lockVal, provVal, supportVal);
        internalSet = true;
        pfrCfgIface->set_property(ufmProvisionedStr, provVal);
        pfrCfgIface->set_property(ufmLockedStr, lockVal);
        pfrCfgIface->set_property(ufmSupportStr, supportVal);
        internalSet = false;
    }
    return;
}

static constexpr const char* postcodeStrProp = "PlatformState";
static constexpr const char* postcodeStrDefault = "Unknown";
static constexpr const char* postcodeDataProp = "Data";
static constexpr const char* postcodeIface =
    "xyz.openbmc_project.State.Boot.Platform";

PfrPostcode::PfrPostcode(sdbusplus::asio::object_server& srv_,
                         std::shared_ptr<sdbusplus::asio::connection>& conn_) :
    server(srv_),
    conn(conn_)
{
    if (getPlatformState(postcode) < 0)
    {
        postcode = 0;
    }

    pfrPostcodeIface =
        server.add_interface("/xyz/openbmc_project/pfr", postcodeIface);

    if (pfrPostcodeIface != nullptr)
    {
        pfrPostcodeIface->register_property(
            postcodeDataProp, postcode,
            // Override set
            [this](const uint8_t req, uint8_t& propertyValue) {
                if (internalSet)
                {
                    if (req != propertyValue)
                    {
                        postcode = req;
                        propertyValue = req;
                        return 1;
                    }
                }
                return 0;
            },
            [this](uint8_t& propertyValue) {
                updatePostcode();
                propertyValue = postcode;
                return propertyValue;
            });

        pfrPostcodeIface->register_property(postcodeStrProp,
                                            std::string(postcodeStrDefault));

        pfrPostcodeIface->initialize();
        auto it = postcodeMap.find(postcode);
        if (it != postcodeMap.end())
        {
            pfrPostcodeIface->set_property(postcodeStrProp, it->second);
        }
    }
}

void PfrPostcode::updatePostcode()
{
    if (pfrPostcodeIface && pfrPostcodeIface->is_initialized())
    {
        if (getPlatformState(postcode) < 0)
        {
            postcode = 0;
        }

        internalSet = true;
        pfrPostcodeIface->set_property(postcodeDataProp, postcode);
        auto it = postcodeMap.find(postcode);
        if (it == postcodeMap.end())
        {
            pfrPostcodeIface->set_property(postcodeStrProp, postcodeStrDefault);
        }
        else
        {
            pfrPostcodeIface->set_property(postcodeStrProp, it->second);
        }
        internalSet = false;
    }
    return;
}

} // namespace pfr
