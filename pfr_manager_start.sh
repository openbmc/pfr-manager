#!/bin/sh

PFR_BUS=4
PFR_ADDR=0x38
PFR_ID_REG=0x00
PFR_PROV_REG=0x0a
pfr_read() {
    [ $# -ne 1 ] && return 1
    local reg=$1
    i2cget -y $PFR_BUS $PFR_ADDR $reg 2>/dev/null
}

pfr_active_mode() {
    # check for 0xde in register file 0
    local id=$(pfr_read $PFR_ID_REG) || return 1
    [ "$id" == "0xde" ] || return 1
    local prov=$(pfr_read $PFR_PROV_REG) || return 1
    prov=$((prov & 0x20))
    [ "$prov" == "32" ] && return 0
    return 1
}

#Read board name
counter=0
board=''

while [ -z $board ]
do
   board=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper  GetSubTreePaths sias "/xyz/openbmc_project/inventory" 0 2 "xyz.openbmc_project.Inventory.Item.Board" "xyz.openbmc_project.Inventory.Item.Chassis" | cut -b 7- | sed 's/.$//')
   if [ $counter -eq 5 ]
   then
       echo "Unable to get board name"
       exit
   fi
   counter=`expr $counter + 1`
   sleep 5
done

#Read PFR property
support=$(busctl get-property xyz.openbmc_project.EntityManager $board/PFR xyz.openbmc_project.Configuration.PFR PFRSupport | cut -b 4- | sed 's/.$//')

echo "PFR Supported=$support"

if [ "$support" == 'true' -a pfr_active_mode ]
then
    echo "Starting PFR manager service"
    /usr/bin/intel-pfr-manager
    exit
else
    echo "Could not start PFR manager service"
    systemctl stop xyz.openbmc_project.PFR.Manager.service
    exit
fi

