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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <phosphor-logging/log.hpp>

#include <experimental/filesystem>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

namespace pfr
{

/** @class I2CFile
 *  @brief Responsible for handling file pointer
 */
class I2CFile
{
  private:
    /** @brief handler for operating on file */
    int fd;

  public:
    I2CFile() = delete;
    I2CFile(const I2CFile&) = delete;
    I2CFile& operator=(const I2CFile&) = delete;
    I2CFile(I2CFile&&) = delete;
    I2CFile& operator=(I2CFile&&) = delete;

    /** @brief Opens i2c device file and sets slave
     *
     *  @param[in] i2cBus       - I2C bus number
     *  @param[in] slaveAddr    - I2C slave address
     *  @param[in] flags        - Flags
     */
    I2CFile(const int& i2cBus, const int& slaveAddr, const int& flags)
    {
        std::string i2cDev = "/dev/i2c-" + std::to_string(i2cBus);

        fd = open(i2cDev.c_str(), flags);
        if (fd < 0)
        {
            throw std::runtime_error("Unable to open i2c device.");
        }

        if (ioctl(fd, I2C_SLAVE_FORCE, slaveAddr) < 0)
        {
            close(fd);
            fd = -1;
            throw std::runtime_error("Unable to set i2c slave address.");
        }
    }

    /** @brief Reads the byte data from I2C dev
     *
     *  @param[in] Offset       -  Offset value
     *  @param[out] byte data      -  Offset value
     */
    uint8_t i2cReadByteData(const uint8_t& offset)
    {
        int value = i2c_smbus_read_byte_data(fd, offset);

        if (value < 0)
        {
            throw std::runtime_error("i2c_smbus_read_byte_data() failed");
        }
        return static_cast<uint8_t>(value);
    }

    /** @brief Reads the block of data from I2C dev
     *
     *  @param[in] Offset       -  Offset value
     *  @param[in] length       -  length value
     *  @param[out] value       -  data pointer
     *  @param[out] bool        -  true or false
     */
    bool i2cReadBlockData(const uint8_t& offset, uint8_t length, uint8_t* value)
    {
        int ret;
        ret = i2c_smbus_read_i2c_block_data(fd, offset, length, value);

        if (ret < 0)
        {
            throw std::runtime_error("i2c_smbus_read_i2c_block_data() failed");
            return false;
        }
        return true;
    }

    /** @brief Writes the byte data to I2C dev
     *
     *  @param[in] Offset       -  Offset value
     *  @param[in] Byte data    -  Data
     */
    void i2cWriteByteData(const uint8_t offset, const uint8_t value)
    {
        int retries = 3;
        while (i2c_smbus_write_byte_data(fd, offset, value) < 0)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "PFR: I2c write failed, retrying....",
                phosphor::logging::entry("COUNT=%d", retries));
            if (!retries--)
            {
                throw std::runtime_error("i2c_smbus_write_byte_data() failed");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return;
    }

    ~I2CFile()
    {
        if (!(fd < 0))
        {
            close(fd);
        }
    }

    auto operator()()
    {
        return fd;
    }
};

} // namespace pfr
