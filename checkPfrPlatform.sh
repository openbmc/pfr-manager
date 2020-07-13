#!/bin/sh

PFR_ID_REG=0x00
PFR_PROV_REG=0x0a

pfr_active_mode() {
    busctl introspect xyz.openbmc_project.EntityManager $board/PFR >&/dev/null || return 1
    address=$(busctl get-property xyz.openbmc_project.EntityManager $board/PFR xyz.openbmc_project.Configuration.PFR Address | cut -b 3-) || return 1
    bus=$(busctl get-property xyz.openbmc_project.EntityManager $board/PFR xyz.openbmc_project.Configuration.PFR Bus | cut -b 3-) || return 1
    #check for 0xde in register file 0
    local id=$(i2cget -y $bus $address $PFR_ID_REG 2>/dev/null) || return 1
    [ "$id" == "0xde" ] || return 1
    local prov=$(i2cget -y $bus $address $PFR_PROV_REG 2>/dev/null) || return 1
    prov=$((prov & 0x20))
    [ "$prov" == "32" ] && return 0
    return 1
}

#Read board names from inventory
counter=0
boards=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper  GetSubTreePaths sias "/xyz/openbmc_project/inventory" 0 2 "xyz.openbmc_project.Inventory.Item.Board" "xyz.openbmc_project.Inventory.Item.Chassis")

while [ -z "$boards" ]
do
   boards=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper  GetSubTreePaths sias "/xyz/openbmc_project/inventory" 0 2 "xyz.openbmc_project.Inventory.Item.Board" "xyz.openbmc_project.Inventory.Item.Chassis")
   if [ $counter -eq 5 ]
   then
       echo "Unable to get board name"
       exit
   fi
   counter=`expr $counter + 1`
   sleep 10
done

#check for Baseboard
for boardIter in $boards
do
    boardIter=$(echo $boardIter | sed "s/\"//g")
    case "$boardIter" in
    *_Baseboard)
    board=$boardIter
    echo "Baseboard is=$board"
    ;;
    esac
done

if pfr_active_mode
then
    echo "Starting PFR manager service"
    /usr/bin/pfr-manager
    exit
else
    echo "PFR not supported on this platforms and so not starting PFR manager service."
    exit
fi
