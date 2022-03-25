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

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <string>

namespace pfr
{

static int i2cBusNumber = 4;
static int i2cSlaveAddress = 56;

enum class ImageType
{
    cpldActive,
    cpldRecovery,
    biosActive,
    biosRecovery,
    bmcActive,
    bmcRecovery,
    afmActive,
    afmRecovery
};

enum class ActionType
{
    recoveryCount,
    recoveryReason,
    panicCount,
    panicReason,
    majorError,
    minorError,
    readRoTRev
};

std::string toHexString(const uint8_t val);
std::string getFirmwareVersion(const ImageType& imgType);
int getProvisioningStatus(bool& ufmLocked, bool& ufmProvisioned,
                          bool& ufmSupport);
int getPlatformState(uint8_t& state);
int readCpldReg(const ActionType& action, uint8_t& value);
std::string readCPLDVersion();
int setBMCBootCheckpoint(const uint8_t checkPoint);
void init(std::shared_ptr<sdbusplus::asio::connection> conn,
          bool& i2cConfigLoaded);

} // namespace pfr
