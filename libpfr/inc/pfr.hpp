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

enum class ImageType
{
    cpldActive,
    cpldRecovery,
    biosActive,
    biosRecovery,
    bmcActive,
    bmcRecovery
};

enum class ActionType
{
    recoveryCount,
    recoveryReason,
    panicCount,
    panicReason,
    majorError,
    minorError
};

std::string toHexString(const uint8_t val);
std::string getFirmwareVersion(const ImageType& imgType);
int getProvisioningStatus(bool& ufmLocked, bool& ufmProvisioned,
                          bool& ufmSupport);
int readCpldReg(const ActionType& action, uint8_t& value);
std::string readCPLDVersion();
int setBMCBootCheckpoint(const uint8_t checkPoint);
void init(std::shared_ptr<sdbusplus::asio::connection> conn, int& i2cFlag);

} // namespace pfr
