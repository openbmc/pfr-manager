#!/bin/sh

PFR_ID_REG=0x00
PFR_PROV_REG=0x0a

pfr_active_mode() {
    busctl introspect xyz.openbmc_project.EntityManager $board/PFR >&/dev/null || return 1
    address=$(busctl get-property xyz.openbmc_project.EntityManager $board/PFR xyz.openbmc_project.Configuration.PFR Address | cut -b 3-) || return 1
    bus=$(busctl get-property xyz.openbmc_project.EntityManager $board/PFR xyz.openbmc_project.Configuration.PFR Bus | cut -b 3-) || return 1
    #check for 0xde in register file 0
    local id=$(i2cget -y $bus $address $PFR_ID_REG 2>/dev/null) || return 1
    [ "$id" == "0xde" ] && return 0
    return 1
}

#Read board name
counter=0
board=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper  GetSubTreePaths sias "/xyz/openbmc_project/inventory" 0 1 "xyz.openbmc_project.Inventory.Item.Board.Motherboard" | cut -b 7- | sed 's/.$//')

while [ -z "$board" ]
do
   board=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper  GetSubTreePaths sias "/xyz/openbmc_project/inventory" 0 1 "xyz.openbmc_project.Inventory.Item.Board.Motherboard" | cut -b 7- | sed 's/.$//')
   if [ $counter -eq 5 ]
   then
       echo "Unable to get board name"
       exit
   fi
   counter=`expr $counter + 1`
   sleep 10
done

echo "Baseboard is=$board"

if pfr_active_mode
then
    echo "Starting PFR manager service"
    /usr/bin/pfr-manager
    exit
else
    echo "PFR not supported on this platforms and so not starting PFR manager service."
    exit
fi
