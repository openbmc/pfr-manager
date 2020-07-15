#!/bin/sh

PFR_ID_REG=0x00
PFR_PROV_REG=0x0a

pfr_active_mode() {
    address=$(busctl get-property xyz.openbmc_project.EntityManager $board/PFR xyz.openbmc_project.Configuration.PFR Address | cut -b 3-) || return 1
    bus=$(busctl get-property xyz.openbmc_project.EntityManager $board/PFR xyz.openbmc_project.Configuration.PFR Bus | cut -b 3-) || return 1
    # Check if properties are read properly
    if [ -z "$address" ] || [ -z "$bus" ]; then
    exit 1
    fi

    #Read register 0 and 0x0a of PFR mailbox
    local id=$(i2cget -y $bus $address $PFR_ID_REG 2>/dev/null) || return 1
    local prov=$(i2cget -y $bus $address $PFR_PROV_REG 2>/dev/null) || return 1

    # Check if registers are read properly
    if [ -z "$id" ] || [ -z "$prov" ]; then
    exit 1
    fi

    # RoT ID
    [ "$id" == "0xde" ] || return 1
    # PFR provision status
    prov=$((prov & 0x20))
    [ "$prov" == "32" ] && return 0
    return 1
}

# Check for EntityManager and ObjectMapper readiness
while :
do
    obj_map=$(systemctl is-active xyz.openbmc_project.ObjectMapper)
    ent_mgr=$(systemctl is-active xyz.openbmc_project.EntityManager)

    if [ "$obj_map" = "active" ] && [ "$ent_mgr" = "active" ]
    then
        echo "EntityManager and ObjectMapper services are active"
    break
    fi
done

echo "ObjectMapper:$obj_map"
echo "EntityManager:$ent_mgr"

#Read board name
counter=0
board=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper  GetSubTreePaths sias "/xyz/openbmc_project/inventory" 0 1 "xyz.openbmc_project.Inventory.Item.Board.Motherboard" | cut -b 7- | sed 's/.$//')

while [ -z "$board" ]
do
   board=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper  GetSubTreePaths sias "/xyz/openbmc_project/inventory" 0 1 "xyz.openbmc_project.Inventory.Item.Board.Motherboard" | cut -b 7- | sed 's/.$//')
   if [ $counter -eq 10 ]
   then
       echo "Unable to get board name"
       exit 1
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
