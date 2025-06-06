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
#include "pfr_mgr.hpp"

#include <systemd/sd-journal.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/unpack_properties.hpp>

namespace pfr
{

static bool i2cConfigLoaded = false;
static int retrCount = 10;

static bool stateTimerRunning = false;
bool bmcBootCompleteChkPointDone = false;
bool unProvChkPointStatus = false;
static constexpr uint8_t bmcBootFinishedChkPoint = 0x09;

std::unique_ptr<boost::asio::steady_timer> stateTimer = nullptr;
std::unique_ptr<boost::asio::steady_timer> initTimer = nullptr;
std::unique_ptr<boost::asio::steady_timer> pfrObjTimer = nullptr;
std::vector<std::unique_ptr<PfrVersion>> pfrVersionObjects;
std::unique_ptr<PfrConfig> pfrConfigObject;
std::unique_ptr<PfrPostcode> pfrPostcodeObject;

// List holds <ObjPath> <ImageType> <VersionPurpose>
static std::vector<std::tuple<std::string, ImageType, std::string>>
    verComponentList = {
        std::make_tuple("bmc_recovery", ImageType::bmcRecovery,
                        versionPurposeBMC),
        std::make_tuple("bios_recovery", ImageType::biosRecovery,
                        versionPurposeHost),
        std::make_tuple("rot_fw_recovery", ImageType::cpldRecovery,
                        versionPurposeOther),
        std::make_tuple("afm_active", ImageType::afmActive,
                        versionPurposeOther),
        std::make_tuple("afm_recovery", ImageType::afmRecovery,
                        versionPurposeOther),
};

// Recovery reason map.
// {<CPLD association>,{<Redfish MessageID>, <Recovery Reason>}}
static const boost::container::flat_map<uint8_t,
                                        std::pair<std::string, std::string>>
    recoveryReasonMap = {
        {0x01,
         {"BIOSFirmwareRecoveryReason",
          "BIOS active image authentication failure"}},
        {0x02,
         {"BIOSFirmwareRecoveryReason",
          "BIOS recovery image authentication failure"}},
        {0x03, {"MEFirmwareRecoveryReason", "ME launch failure"}},
        {0x04, {"BIOSFirmwareRecoveryReason", "ACM launch failure"}},
        {0x05, {"BIOSFirmwareRecoveryReason", "IBB launch failure"}},
        {0x06, {"BIOSFirmwareRecoveryReason", "OBB launch failure"}},
        {0x07,
         {"BMCFirmwareRecoveryReason",
          "BMC active image authentication failure"}},
        {0x08,
         {"BMCFirmwareRecoveryReason",
          "BMC recovery image authentication failure"}},
        {0x09, {"BMCFirmwareRecoveryReason", "BMC launch failure"}},
        {0x0A, {"CPLDFirmwareRecoveryReason", "CPLD watchdog expired"}},
        {0x0B, {"BMCFirmwareRecoveryReason", "BMC attestation failure"}},
        {0x0C, {"FirmwareResiliencyError", "CPU0  attestation failure"}},
        {0x0D, {"FirmwareResiliencyError", "CPU1  attestation failure"}}};

// Panic Reason map.
// {<CPLD association>, {<Redfish MessageID>, <Panic reason> })
static const boost::container::flat_map<uint8_t,
                                        std::pair<std::string, std::string>>
    panicReasonMap = {
        {0x01, {"BIOSFirmwarePanicReason", "BIOS update intent"}},
        {0x02, {"BMCFirmwarePanicReason", "BMC update intent"}},
        {0x03, {"BMCFirmwarePanicReason", "BMC reset detected"}},
        {0x04, {"BMCFirmwarePanicReason", "BMC watchdog expired"}},
        {0x05, {"MEFirmwarePanicReason", "ME watchdog expired"}},
        {0x06, {"BIOSFirmwarePanicReason", "ACM/IBB/OBB WDT expired"}},
        {0x09,
         {"BIOSFirmwarePanicReason",
          "ACM or IBB or OBB authentication failure"}},
        {0x0A, {"FirmwareResiliencyError", "Attestation failure"}}};

// Firmware resiliency major map.
// {<CPLD association>, {<Redfish MessageID>, <Error reason> })
static const boost::container::flat_map<uint8_t,
                                        std::pair<std::string, std::string>>
    majorErrorCodeMap = {
        {0x01,
         {"BMCFirmwareResiliencyError", "BMC image authentication failed"}},
        {0x02,
         {"BIOSFirmwareResiliencyError", "BIOS image authentication failed"}},
        {0x03,
         {"BIOSFirmwareResiliencyError", "in-band and oob update failure"}},
        {0x04, {"BMCFirmwareResiliencyError", "Communication setup failed"}},
        {0x05,
         {"FirmwareResiliencyError",
          "Attestation measurement mismatch-Attestation failure"}},
        {0x06, {"FirmwareResiliencyError", "Attestation challenge timeout"}},
        {0x07, {"FirmwareResiliencyError", "SPDM protocol timeout"}},
        {0x08, {"FirmwareResiliencyError", "I2c Communication failure"}},
        {0x09,
         {"CPLDFirmwareResiliencyError",
          "Combined CPLD authentication failure"}},
        {0x0A, {"CPLDFirmwareResiliencyError", "Combined CPLD update failure"}},
        {0x0B,
         {"CPLDFirmwareResiliencyError", "Combined CPLD recovery failure"}},
        {0x10, {"FirmwareResiliencyError", "Image copy Failed"}}};

static void updateDbusPropertiesCache()
{
    for (const auto& pfrVerObj : pfrVersionObjects)
    {
        pfrVerObj->updateVersion();
    }

    // Update provisoningStatus properties
    pfrConfigObject->updateProvisioningStatus();

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "PFR Manager service cache data updated.");
}

static void logLastRecoveryEvent()
{
    uint8_t reason = 0;
    if (0 != readCpldReg(ActionType::recoveryReason, reason))
    {
        return;
    }

    auto it = recoveryReasonMap.find(reason);
    if (it == recoveryReasonMap.end())
    {
        // No matching found. So just return without logging event.
        return;
    }
    std::string msgId = "OpenBMC.0.1." + it->second.first;
    sd_journal_send("MESSAGE=%s", "Platform firmware recovery occurred.",
                    "PRIORITY=%i", LOG_WARNING, "REDFISH_MESSAGE_ID=%s",
                    msgId.c_str(), "REDFISH_MESSAGE_ARGS=%s",
                    it->second.second.c_str(), NULL);
}

static void logLastPanicEvent()
{
    uint8_t reason = 0;
    if (0 != readCpldReg(ActionType::panicReason, reason))
    {
        return;
    }

    auto it = panicReasonMap.find(reason);
    if (it == panicReasonMap.end())
    {
        // No matching found. So just return without logging event.
        return;
    }

    std::string msgId = "OpenBMC.0.1." + it->second.first;
    sd_journal_send("MESSAGE=%s", "Platform firmware panic occurred.",
                    "PRIORITY=%i", LOG_WARNING, "REDFISH_MESSAGE_ID=%s",
                    msgId.c_str(), "REDFISH_MESSAGE_ARGS=%s",
                    it->second.second.c_str(), NULL);
}

static void logResiliencyErrorEvent(const uint8_t majorErrorCode,
                                    const uint8_t minorErrorCode)
{
    uint8_t cpldRoTRev = 0;
    if (0 != readCpldReg(ActionType::readRoTRev, cpldRoTRev))
    {
        return;
    }

    auto it = majorErrorCodeMap.find(majorErrorCode);
    if (cpldRoTRev == 0x02)
    {
        auto itRev2 = majorErrorCodeMapRev2.find(majorErrorCode);
        if (itRev2 != majorErrorCodeMapRev2.end())
        {
            it = itRev2;
        }
        else if (it == majorErrorCodeMap.end())
        {
            // No matching found. So just return without logging event.
            return;
        }
    }
    else if (it == majorErrorCodeMap.end())
    {
        // No matching found. So just return without logging event.
        return;
    }

    std::string errorStr =
        it->second.second + "(MinorCode:0x" + toHexString(minorErrorCode) + ")";
    std::string msgId = "OpenBMC.0.1." + it->second.first;
    sd_journal_send(
        "MESSAGE=%s", "Platform firmware resiliency error occurred.",
        "PRIORITY=%i", LOG_ERR, "REDFISH_MESSAGE_ID=%s", msgId.c_str(),
        "REDFISH_MESSAGE_ARGS=%s", errorStr.c_str(), NULL);
}

static void handleLastCountChange(
    std::shared_ptr<sdbusplus::asio::connection> conn, std::string eventName,
    uint8_t currentCount)
{
    sdbusplus::asio::setProperty(
        *conn, "xyz.openbmc_project.Settings",
        "/xyz/openbmc_project/pfr/last_events",
        "xyz.openbmc_project.PFR.LastEvents", eventName, currentCount,
        [](boost::system::error_code ec) {
            if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "PFR: Unable to update currentCount",
                    phosphor::logging::entry("MSG=%s", ec.message().c_str()));
                return;
            }
        });
    return;
}

static void checkAndLogEvents(
    std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    sdbusplus::asio::getAllProperties(
        *conn, "xyz.openbmc_project.Settings",
        "/xyz/openbmc_project/pfr/last_events",
        "xyz.openbmc_project.PFR.LastEvents",
        [conn](
            boost::system::error_code ec,
            const std::vector<
                std::pair<std::string, std::variant<std::monostate, uint8_t>>>&
                properties) {
            if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "PFR: Unable get PFR last events",
                    phosphor::logging::entry("MSG=%s", ec.message().c_str()));
                return;
            }
            uint8_t lastRecoveryCount = 0;
            uint8_t lastPanicCount = 0;
            uint8_t lastMajorErr = 0;
            uint8_t lastMinorErr = 0;

            try
            {
                sdbusplus::unpackProperties(
                    properties, "lastRecoveryCount", lastRecoveryCount,
                    "lastPanicCount", lastPanicCount, "lastMajorErr",
                    lastMajorErr, "lastMinorErr", lastMinorErr);
            }
            catch (const sdbusplus::exception::UnpackPropertyError& error)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "PFR: Unpack error",
                    phosphor::logging::entry("MSG=%s", error.what()));
                return;
            }

            uint8_t currPanicCount = 0;
            if (0 == readCpldReg(ActionType::panicCount, currPanicCount))
            {
                if (lastPanicCount != currPanicCount)
                {
                    // Update cached data to dbus and log redfish
                    // event by reading reason.
                    handleLastCountChange(conn, "lastPanicCount",
                                          currPanicCount);
                    if (currPanicCount)
                    {
                        logLastPanicEvent();
                    }
                }
            }

            uint8_t currRecoveryCount = 0;
            if (0 == readCpldReg(ActionType::recoveryCount, currRecoveryCount))
            {
                if (lastRecoveryCount != currRecoveryCount)
                {
                    // Update cached data to dbus and log redfish
                    // event by reading reason.
                    handleLastCountChange(conn, "lastRecoveryCount",
                                          currRecoveryCount);
                    if (currRecoveryCount)
                    {
                        logLastRecoveryEvent();
                    }
                }
            }

            uint8_t majorErr = 0;
            uint8_t minorErr = 0;
            if ((0 == readCpldReg(ActionType::majorError, majorErr)) &&
                (0 == readCpldReg(ActionType::minorError, minorErr)))
            {
                if ((lastMajorErr != majorErr) || (lastMinorErr != minorErr))
                {
                    // Update cached data to dbus and log redfish event by
                    // reading reason.
                    handleLastCountChange(conn, "lastMajorErr", majorErr);
                    handleLastCountChange(conn, "lastMinorErr", minorErr);
                    if (majorErr && minorErr)
                    {
                        logResiliencyErrorEvent(majorErr, minorErr);
                    }
                }
            }
        });
}

static void monitorPlatformStateChange(
    sdbusplus::asio::object_server& server,
    std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    constexpr size_t pollTimeout = 10; // seconds
    stateTimer->expires_after(std::chrono::seconds(pollTimeout));
    stateTimer->async_wait(
        [&server, &conn](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                // Timer reset.
                return;
            }
            if (ec)
            {
                // Platform State Monitor - Timer cancelled.
                return;
            }
            checkAndLogEvents(conn);
            monitorPlatformStateChange(server, conn);
        });
}

void checkAndSetCheckpoint(sdbusplus::asio::object_server& server,
                           std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    // Check whether systemd completed all the loading.
    conn->async_method_call(
        [&server, &conn](boost::system::error_code ec,
                         const std::variant<uint64_t>& value) {
            if (!ec)
            {
                if (std::get<uint64_t>(value))
                {
                    phosphor::logging::log<phosphor::logging::level::INFO>(
                        "BMC boot completed. Setting checkpoint 9.");
                    if (!bmcBootCompleteChkPointDone)
                    {
                        setBMCBootCompleteChkPoint(bmcBootFinishedChkPoint);
                    }
                    return;
                }
            }
            else
            {
                // Failed to get data from systemd. System might not
                // be ready yet. Attempt again for data.
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "aync call failed to get FinishTimestamp.",
                    phosphor::logging::entry("MSG=%s", ec.message().c_str()));
            }
            // FIX-ME: Latest up-stream sync caused issue in receiving
            // StartupFinished signal. Unable to get StartupFinished signal
            // from systemd1 hence using poll method too, to trigger it
            // properly.
            constexpr size_t pollTimeout = 10; // seconds
            initTimer->expires_after(std::chrono::seconds(pollTimeout));
            initTimer->async_wait(
                [&server, &conn](const boost::system::error_code& ec) {
                    if (ec == boost::asio::error::operation_aborted)
                    {
                        // Timer reset.
                        phosphor::logging::log<phosphor::logging::level::INFO>(
                            "Set boot Checkpoint - Timer aborted or stopped.");
                        return;
                    }
                    if (ec)
                    {
                        phosphor::logging::log<phosphor::logging::level::ERR>(
                            "Set boot Checkpoint - async wait error.");
                        return;
                    }
                    checkAndSetCheckpoint(server, conn);
                });
        },
        "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.DBus.Properties", "Get",
        "org.freedesktop.systemd1.Manager", "FinishTimestamp");
}

void monitorSignals(sdbusplus::asio::object_server& server,
                    std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    // Monitor Boot finished signal and set the checkpoint 9 to
    // notify CPLD about BMC boot finish.
    auto bootFinishedSignal = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',"
        "member='StartupFinished',path='/org/freedesktop/systemd1',"
        "interface='org.freedesktop.systemd1.Manager'",
        [&server, &conn](sdbusplus::message_t& msg) {
            if (!bmcBootCompleteChkPointDone)
            {
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "BMC boot completed(StartupFinished). Setting "
                    "checkpoint 9.");
                setBMCBootCompleteChkPoint(bmcBootFinishedChkPoint);
            }
        });
    checkAndSetCheckpoint(server, conn);

    // Capture the Chassis state and Start the monitor timer
    // if state changed to 'On'. Run timer until  OS boot.
    // Stop timer if state changed to 'Off'.
    static auto matchChassisState = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',member='PropertiesChanged', "
        "interface='org.freedesktop.DBus.Properties', "
        "sender='xyz.openbmc_project.State.Chassis', "
        "arg0namespace='xyz.openbmc_project.State.Chassis'",
        [&server, &conn](sdbusplus::message_t& message) {
            std::string intfName;
            std::map<std::string, std::variant<std::string>> properties;
            message.read(intfName, properties);

            const auto it = properties.find("CurrentPowerState");
            if (it != properties.end())
            {
                const std::string* state =
                    std::get_if<std::string>(&it->second);
                if (state != nullptr)
                {
                    if ((*state ==
                         "xyz.openbmc_project.State.Chassis.PowerState.On") &&
                        (!stateTimerRunning))
                    {
                        stateTimerRunning = true;
                        monitorPlatformStateChange(server, conn);
                    }
                    else if ((*state == "xyz.openbmc_project.State.Chassis."
                                        "PowerState.Off") &&
                             (stateTimerRunning))
                    {
                        stateTimer->cancel();
                        checkAndLogEvents(conn);
                        stateTimerRunning = false;
                    }
                }

                // Update the D-Bus properties when chassis state changes.
                updateDbusPropertiesCache();
            }
        });

    // Capture the Host state and Start the monitor timer
    // if state changed to 'Running'. Run timer until OS boot.
    // Stop timer if state changed to 'Off'.
    static auto matchHostState = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',member='PropertiesChanged', "
        "interface='org.freedesktop.DBus.Properties', "
        "sender='xyz.openbmc_project.State.Chassis', "
        "arg0namespace='xyz.openbmc_project.State.Host'",
        [&server, &conn](sdbusplus::message_t& message) {
            std::string intfName;
            std::map<std::string, std::variant<std::string>> properties;
            message.read(intfName, properties);

            const auto it = properties.find("CurrentHostState");
            if (it != properties.end())
            {
                const std::string* state =
                    std::get_if<std::string>(&it->second);
                if (state != nullptr)
                {
                    if ((*state ==
                         "xyz.openbmc_project.State.Host.HostState.Running") &&
                        (!stateTimerRunning))
                    {
                        stateTimerRunning = true;
                        monitorPlatformStateChange(server, conn);
                    }
                    else if (((*state == "xyz.openbmc_project.State.Host."
                                         "HostState.Off") ||
                              (*state == "xyz.openbmc_project.State.Host."
                                         "HostState.Quiesced")) &&
                             (stateTimerRunning))
                    {
                        stateTimer->cancel();
                        checkAndLogEvents(conn);
                        stateTimerRunning = false;
                    }
                }

                // Update the D-Bus properties when host state changes.
                updateDbusPropertiesCache();
            }
        });

    // Capture the OS state change and stop monitor timer
    // if OS boots completely or becomes Inactive.
    // start timer in other cases to mnitor states.
    static auto matchOsState = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',member='PropertiesChanged', "
        "interface='org.freedesktop.DBus.Properties', "
        "sender='xyz.openbmc_project.State.Chassis', "
        "arg0namespace='xyz.openbmc_project.State.OperatingSystem.Status'",
        [&server, &conn](sdbusplus::message_t& message) {
            std::string intfName;
            std::map<std::string, std::variant<std::string>> properties;
            message.read(intfName, properties);

            const auto it = properties.find("OperatingSystemState");
            if (it != properties.end())
            {
                const std::string* state =
                    std::get_if<std::string>(&it->second);
                if (state != nullptr)
                {
                    // The short strings "BootComplete" and "Standby" are
                    // deprecated in favor of the full enum strings
                    // Support for the short strings will be removed in the
                    // future.
                    if (((*state == "BootComplete") ||
                         (*state == "xyz.openbmc_project.State.OperatingSystem."
                                    "Status.OSStatus.BootComplete") ||
                         (*state == "Inactive") ||
                         (*state == "xyz.openbmc_project.State.OperatingSystem."
                                    "Status.OSStatus.Inactive")) &&
                        (stateTimerRunning))
                    {
                        stateTimer->cancel();
                        checkAndLogEvents(conn);
                        stateTimerRunning = false;
                    }
                    else if (!stateTimerRunning)
                    {
                        stateTimerRunning = true;
                        monitorPlatformStateChange(server, conn);
                    }
                }
            }
        });

    // First time, check and log events if any.
    checkAndLogEvents(conn);
}

static void updateCPLDversion(std::shared_ptr<sdbusplus::asio::connection> conn)
{
    std::string cpldVersion = pfr::readCPLDVersion();
    lg2::info("VERSION INFO - rot_fw_active - {VER}", "VER", cpldVersion);
    conn->async_method_call(
        [](const boost::system::error_code ec) {
            if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Unable to update rot_fw_active version",
                    phosphor::logging::entry("MSG=%s", ec.message().c_str()));
                return;
            }
        },
        "xyz.openbmc_project.Settings",
        "/xyz/openbmc_project/software/rot_fw_active",
        "org.freedesktop.DBus.Properties", "Set",
        "xyz.openbmc_project.Software.Version", "Version",
        std::variant<std::string>(cpldVersion));
    return;
}

void checkPfrInterface(std::shared_ptr<sdbusplus::asio::connection> conn,
                       sdbusplus::asio::object_server& server)
{
    if (!i2cConfigLoaded)
    {
        init(conn, i2cConfigLoaded);
        if (retrCount > 0)
        {
            // pfr object not loaded yet. query again.
            return;
        }
        else
        {
            // Platform does not contain pfr object. Stop the service.
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "Platform does not support PFR, hence stop the "
                "service.");
            std::exit(EXIT_SUCCESS);
            return;
        }
    }
    else
    {
        retrCount = 0;

        bool locked = false;
        bool prov = false;
        bool support = false;
        pfr::getProvisioningStatus(locked, prov, support);
        if (support && prov)
        {
            // pfr provisioned.
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "PFR Supported.");
            return;
        }
        else
        {
            unProvChkPointStatus = true;
            pfr::monitorSignals(server, conn);
        }
    }
}
void checkPFRandAddObjects(sdbusplus::asio::object_server& server,
                           std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    checkPfrInterface(conn, server);

    constexpr size_t timeout = 10; // seconds
    pfrObjTimer->expires_after(std::chrono::seconds(timeout));
    pfrObjTimer->async_wait([&conn,
                             &server](const boost::system::error_code& ec) {
        if (ec)
        {
            if (ec == boost::asio::error::operation_aborted)
            {
                // Timer reset.
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "pfr object found. Hence Object Timer aborted or stopped.");
            }
            else
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "pfr object timer error.");
            }
        }
        if (retrCount > 0)
        {
            checkPFRandAddObjects(server, conn);
        }
        else
        {
            pfr::monitorSignals(server, conn);

            // Update the D-Bus properties.
            updateDbusPropertiesCache();
            // Update CPLD Version to rot_fw_active object in settings.
            updateCPLDversion(conn);
        }
        retrCount--;
    });
}
} // namespace pfr

int main()
{
    // setup connection to dbus
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    pfr::stateTimer = std::make_unique<boost::asio::steady_timer>(io);
    pfr::initTimer = std::make_unique<boost::asio::steady_timer>(io);
    pfr::pfrObjTimer = std::make_unique<boost::asio::steady_timer>(io);
    auto server = sdbusplus::asio::object_server(conn, true);
    pfr::init(conn, pfr::i2cConfigLoaded);

    pfr::checkPFRandAddObjects(server, conn);

    // Update CPLD Version to rot_fw_active object in settings.
    pfr::updateCPLDversion(conn);

    server.add_manager("/xyz/openbmc_project/pfr");

    // Create PFR attributes object and interface
    pfr::pfrConfigObject = std::make_unique<pfr::PfrConfig>(server, conn);

    // Create Software objects using Versions interface
    for (const auto& entry : pfr::verComponentList)
    {
        pfr::pfrVersionObjects.emplace_back(std::make_unique<pfr::PfrVersion>(
            server, conn, std::get<0>(entry), std::get<1>(entry),
            std::get<2>(entry)));
    }

    if (pfr::pfrConfigObject)
    {
        pfr::pfrConfigObject->updateProvisioningStatus();
        if (pfr::pfrConfigObject->getPfrProvisioned())
        {
            pfr::pfrPostcodeObject =
                std::make_unique<pfr::PfrPostcode>(server, conn);
        }
    }

    conn->request_name("xyz.openbmc_project.PFR.Manager");
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Intel PFR service started successfully");
    io.run();

    return 0;
}
