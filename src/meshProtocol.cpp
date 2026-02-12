


//========================================= INCLUDES ==============================================//
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_system.h"

#include "meshProtocol.h"


//====================================== VARIABLES =============================================//
uint8_t meshPacket_cacheIndex = 0;
uint16_t meshPacket_messageCounter = 0;
uint32_t meshPacket_deviceLastSeen[MAX_IOT_DEVICES];

QueueHandle_t meshPacket_Queue;

uint8_t meshPacket_broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};


meshPacketCache_t meshPacketCache[MESH_PACKET_CACHE_SIZE];
knownPeers_t knownPeers[MAX_PEERS];
routingTable_t routingTable[MESH_PACKET_MAX_ROUTES];
PendingAck_t pending[MESH_PACKET_PENDING_ACKS];


//========================================= FUNCTIONS ==============================================//
/*void packetProcessor_main(void *pvParameters)
{
  while(1)
  {
    if(InternetGateway_systemState == SYSTEM_STATE_ACTIVE)
    {
      //- Process available mesh packet(s).
      meshPacket_processPackets((uint8_t[]){DEVICE_ID_INTERNET_GATEWAY, DEVICE_ID_DATABASE}, 2, 2);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}*/

esp_err_t meshPacket_init(uint8_t wifiChannel)
{
  //- Init ESP-NOW.
  if(esp_now_init() != ESP_OK)
  {
    return ESP_FAIL;
  }

  meshPacket_Queue = xQueueCreate(MESH_PACKET_QUEUE_LENGTH, sizeof(meshPacketQueue_t));
  if(meshPacket_Queue == NULL)
  {
    return ESP_FAIL;
  }

  //- NOTE: Register callbacks after queue in case a packet arrives before the queue exists.
  esp_now_register_send_cb(esp_now_send_cb_t(meshPacket_OnDataSent)); //- Register Send callback to get the status of trasnmitted packet.
  esp_now_register_recv_cb(esp_now_recv_cb_t(meshPacket_OnDataRecv)); //- Register callback to get received packet info.

  return meshProtocol_addPeer(meshPacket_broadcastAddress, DEVICE_ID_BROADCAST, wifiChannel); //- Finally let's add broadcast pair.
}

esp_err_t meshProtocol_addPeer(const uint8_t *mac, uint8_t nodeID, uint8_t wifiChannel)
{
  if(esp_now_is_peer_exist(mac)) return ESP_OK;
  if(nodeID == DEVICE_ID_INVALID) return ESP_FAIL; //- Reject some IDs from being added as a peer.

  int idx = -1; 
  for(uint8_t i = 0; i < MAX_PEERS; i++)
  {
    if(knownPeers[i].inUse == false) //- Find available slot.
    {
      idx = i;
      break;
    }
  }
  if(idx == -1) return ESP_FAIL; //- Return on no available slot.

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = wifiChannel;
  peerInfo.encrypt = false;

  if(esp_now_add_peer(&peerInfo) == ESP_OK)
  {
      knownPeers[idx].inUse = true;
      knownPeers[idx].nodeID = nodeID;
      memcpy(knownPeers[idx].MAC, mac, 6);
      return ESP_OK;
  }
  return ESP_FAIL;
}

esp_err_t meshProtocol_removePeer(uint8_t *mac)
{
  if(!esp_now_is_peer_exist(mac)) return ESP_FAIL;
  for(uint8_t i = 0; i < MAX_PEERS; i++)
  {
    if(knownPeers[i].inUse == false) continue; //- Skip unused slots.
    if(memcmp(mac, knownPeers[i].MAC, 6) == 0) 
    {
      if(esp_now_del_peer(mac) == ESP_OK)
      {
        knownPeers[i].inUse = false;
        knownPeers[i].nodeID = 0;
        memset(knownPeers[i].MAC, 0, 6);
        return ESP_OK;
      }
      return ESP_FAIL;
    }
  }
  return ESP_FAIL;
}

/*uint8_t *meshPacket_findMACbyID(uint8_t deviceID)
{
  for(uint8_t i = 0; i < MAX_PEERS; i++)
  {
    if(knownPeers[i].inUse && knownPeers[i].nodeID == deviceID) return knownPeers[i].MAC;
  }
  return NULL;
}*/

int meshPacket_routeFind(uint8_t destID)
{
  for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES; i++)
  {
    if(routingTable[i].inUse && routingTable[i].destinationID == destID) return i;
  }
  return -1;
}

esp_err_t meshPacket_routeAdd(uint8_t destID, const uint8_t *nextHopMAC, int8_t RSSI) //- ToDo: Fix limitation of "want last-seen route wins".
{
  int idx = meshPacket_routeFind(destID);
  if(idx < 0) //- No existing route, try to allocate a new slot.
  {
    for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES; i++)
    {
      if(routingTable[i].inUse == false)
      {
        idx = i;
        break;
      }
    }
  }
  if(idx < 0) return ESP_FAIL; //- Return when no route is found AND table is full.

  routingTable[idx].inUse = true;
  routingTable[idx].destinationID = destID;
  routingTable[idx].lastSeen = millis();
  routingTable[idx].lastRSSI = RSSI;
  memcpy(routingTable[idx].nextHopMAC, nextHopMAC, 6);
  
  meshProtocol_addPeer(nextHopMAC, destID, 0); //- Let's also try to add new peer device. Fail is expected since MESH_PACKET_MAX_ROUTES > MAX_PEERS.
  return ESP_OK;
}

void meshPacket_routeAge()
{
  uint32_t now = millis();
  for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES; i++)
  {
    if(routingTable[i].inUse && (now - routingTable[i].lastSeen > MESH_PACKET_NODE_EXPIRE_TIME_MS))
    {
      routingTable[i].inUse = false; //- Mark as expired.

      #ifdef ENABLE_DEBUG_MESSAGES
      Serial.printf("[MESH][INFO]: Route for D%02d expired\n", routingTable[i].destinationID);
      #endif
    }
  }
}

void meshPacket_rememberPacket(uint8_t sourceID, uint16_t uniqueIdentifier)
{
  meshPacketCache[meshPacket_cacheIndex].sourceID = sourceID;
  meshPacketCache[meshPacket_cacheIndex].uniqueIdentifier = uniqueIdentifier;
  //meshPacketCache[meshPacket_cacheIndex].lastSeen = millis();
  meshPacket_cacheIndex = (meshPacket_cacheIndex + 1) % MESH_PACKET_CACHE_SIZE;

  if(sourceID < MAX_IOT_DEVICES)
  {
    meshPacket_deviceLastSeen[sourceID] = millis();
  }
}

bool meshPacket_isPacketSeen(uint8_t sourceID, uint16_t uniqueIdentifier)
{
  for(uint8_t i = 0; i < MESH_PACKET_CACHE_SIZE; i++)
  {
    if(meshPacketCache[i].sourceID == sourceID && meshPacketCache[i].uniqueIdentifier == uniqueIdentifier) return 1;
  }
  return 0;
}

uint8_t meshPacket_getActiveDeviceCount(uint32_t lastSeenDeviceThreshold_ms)
{
  uint8_t activeDevicesCount = 0;
  uint32_t currentMillis = millis();
  for(uint8_t i = 0; i < MAX_IOT_DEVICES; i++)
  {
    if((currentMillis - meshPacket_deviceLastSeen[i] < lastSeenDeviceThreshold_ms) && meshPacket_deviceLastSeen[i] != 0)
    {
      activeDevicesCount++;
    }
  }
  return activeDevicesCount;
}

void meshPacket_markDelivered(uint16_t uniqueID, uint8_t fromNode)
{
  for(uint8_t i = 0; i < MESH_PACKET_PENDING_ACKS; i++)
  {
    if(pending[i].uniqueID == uniqueID && pending[i].destID == fromNode)
    {
      #ifdef ENABLE_DEBUG_MESSAGES
      Serial.printf("[MESH][INFO]: ACK received from S%02u\n", fromNode);
      #endif

      memset(&pending[i], 0, sizeof(PendingAck_t)); //- Clear slot.
      break; //- Stop at first match.
    }
  }
}

void meshPacket_addPendingAck(uint16_t uniqueID, uint8_t destID, meshPacket_t *packet)
{
  int32_t freeIndex = -1;
  uint8_t oldestIndex = 0;
  unsigned long oldestTime = millis();

  for(uint8_t i = 0; i < MESH_PACKET_PENDING_ACKS; i++)
  {
    if(pending[i].uniqueID == 0) // Free slot
    {
      freeIndex = i;
      break;
    }
    else
    {
      // Track oldest slot in case no free one exists
      if(pending[i].lastSend <= oldestTime)
      {
        oldestTime = pending[i].lastSend;
        oldestIndex = i;
      }
    }
  }

  // Choose either a free slot or replace the oldest one
  int targetIndex = (freeIndex != -1) ? freeIndex : oldestIndex;

  pending[targetIndex].uniqueID = uniqueID;
  pending[targetIndex].destID   = destID;
  pending[targetIndex].retries  = 0;
  pending[targetIndex].lastSend = millis();
  pending[targetIndex].packet = *packet;
}

/*
void checkRetransmissions()
{
  unsigned long now = millis();
  for (int i = 0; i < MESH_PACKET_PENDING_ACKS; i++)
  {
    if(pending[i].uniqueID == 0) continue; // skip empty slots
    if(pending[i].destID != DEVICE_ID_WATER_BOILER && pending[i].destID != DEVICE_ID_INTERNET_GATEWAY) continue; //ToDo: Laikinai

    if((now - pending[i].lastSend > 100) && (pending[i].retries < 3))
    {
      Serial.printf("Retrying message %u to node %u (attempt %u)\n", pending[i].uniqueID, pending[i].destID, pending[i].retries + 1);

      //meshPacket_retransmitPacket(meshPacket_t *localPacket)
      esp_now_send(meshPacket_broadcastAddress, (uint8_t *)&pending[i].packet, pending[i].packet.payloadLength + MESH_PACKET_HEADER_LENGTH);

      pending[i].retries++;
      pending[i].lastSend = now;
    }
  }
}*/

/*esp_err_t meshPacket_sendBeacon(uint8_t sourceID) //- ToDo: just for testing. It can be simplified by using meshPacket_sendMessage().
{
  meshPacket_t beaconPacket;
  beaconPacket.sourceID = sourceID;
  beaconPacket.destinationID = DEVICE_ID_BROADCAST;
  beaconPacket.packetType = PACKET_TYPE_BEACON;
  beaconPacket.payloadLength = 6;
  beaconPacket.TTL = MESH_PACKET_HOP_LIMIT; 
  beaconPacket.uniqueIdentifier = __atomic_fetch_add(&meshPacket_messageCounter, 1, __ATOMIC_RELAXED); //- Atomic increment of 16-bit counter (safe across ISRs/tasks).
  memcpy(beaconPacket.payload, (uint8_t *)"BEACON", beaconPacket.payloadLength);

  //- Remember mesh packet, so it could be ignored immidiately.
  meshPacket_rememberPacket(beaconPacket.sourceID, beaconPacket.uniqueIdentifier);
  return esp_now_send(meshPacket_broadcastAddress, (uint8_t*)&beaconPacket, beaconPacket.payloadLength + MESH_PACKET_HEADER_LENGTH); 
}*/

void meshPacket_retransmitPacket(meshPacket_t *localPacket, const uint8_t *MAC)
{
  if(localPacket->TTL > 0) //- Forward only if TTL > 0.
  {
    localPacket->TTL--; //- Decrement TTL before sending.
    vTaskDelay(pdMS_TO_TICKS((esp_random() % 5) + 1)); //- Slight delay to avoid saturating network.
    esp_now_send(MAC, (uint8_t *)localPacket, localPacket->payloadLength + MESH_PACKET_HEADER_LENGTH);
  }
}

esp_err_t meshPacket_sendMessage(uint8_t sourceID, uint8_t destinationID, uint8_t packetType, const uint8_t *payload, uint8_t payloadLength, bool loopback, int32_t forceUID)
{
  if(payload == NULL && payloadLength > 0) return ESP_ERR_INVALID_ARG; //- NULL only allowed if length == 0.
  if(payloadLength > MAXIMUM_PACKET_LENGTH - MESH_PACKET_HEADER_LENGTH) return ESP_ERR_INVALID_ARG; //- Return error.

  meshPacket_t sendPacket;
  sendPacket.sourceID = sourceID;
  sendPacket.destinationID = destinationID;
  sendPacket.packetType = packetType;
  sendPacket.payloadLength = payloadLength;
  sendPacket.TTL = MESH_PACKET_HOP_LIMIT; 
  sendPacket.uniqueIdentifier = (forceUID == -1) ? __atomic_fetch_add(&meshPacket_messageCounter, 1, __ATOMIC_RELAXED) : (uint16_t)forceUID; //- Atomic increment of 16-bit counter (safe across ISRs/tasks).
  
  if(payloadLength > 0) memcpy(sendPacket.payload, payload, payloadLength); //- Guard agaisnt payload == NULL. 
  
  if(loopback == true) //- Loopback. NOTE: "sourceID == destinationID" can be used but is less flexible.
  {
    if(meshPacket_handlePacketCallback != NULL) //- Check if function was declared externally.
    {
      meshPacket_handlePacketCallback(&sendPacket); //- Go directly to packet handling callback. Loopback doesn't require packets to be remembered, processed.
    }
    return ESP_NOW_SEND_SUCCESS;
  }

  //- Remember mesh packet, so it could be ignored immidiately.
  meshPacket_rememberPacket(sendPacket.sourceID, sendPacket.uniqueIdentifier);

  if(packetType != PACKET_TYPE_ACKNOWLEDGEMENT)
  {
    meshPacket_addPendingAck(sendPacket.uniqueIdentifier, sendPacket.destinationID, &sendPacket);
  }

  int idx = meshPacket_routeFind(destinationID);
  const uint8_t *mac = (idx >= 0) ? routingTable[idx].nextHopMAC : meshPacket_broadcastAddress;

  //- Log details, including MAC.
  #ifdef ENABLE_DEBUG_MESSAGES
  Serial.printf("[MESH][INFO]: Sending packet S%02d, D%02d, T%02d, L%02d, UID%05d\n", sourceID, destinationID, packetType, payloadLength, sendPacket.uniqueIdentifier);
  Serial.printf("[MESH][INFO]: First hop D%02d / MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", (idx >= 0) ? routingTable[idx].destinationID : DEVICE_ID_BROADCAST, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  #endif

  return esp_now_send(mac, (const uint8_t*)&sendPacket, payloadLength + MESH_PACKET_HEADER_LENGTH);
}

void meshPacket_OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  /*for(uint8_t i = 0; i < MAX_IOT_DEVICES; i++)
  {
    if(deviceTelemetry[i].device_MACaddr[0] == mac_addr[0] && deviceTelemetry[i].device_MACaddr[1] == mac_addr[1] && deviceTelemetry[i].device_MACaddr[2] == mac_addr[2] && deviceTelemetry[i].device_MACaddr[3] == mac_addr[3] && deviceTelemetry[i].device_MACaddr[4] == mac_addr[4] && deviceTelemetry[i].device_MACaddr[5] == mac_addr[5])
    {
      Serial.printf("[ESP-NOW][INFO]: %s control packet sent", deviceTelemetry[i].deviceName);
      Serial.println(status == ESP_NOW_SEND_SUCCESS ? " SUCCESFULLY!" : " UNSUCCESFULLY!");

      if(status != ESP_NOW_SEND_SUCCESS) setError(ERROR_COMM_FAILURE);
    }
  }*/
}

void meshPacket_OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len)
{
  if(incomingData == NULL || len <= 0) return; //- Validate arguments. It's probably optional but I don't want to risk it :)
  if(len > sizeof(meshPacket_t)) return;
  if(len < MESH_PACKET_HEADER_LENGTH) return;

  meshPacketQueue_t tmpPacket;
  memset(&tmpPacket, 0, sizeof(tmpPacket)); //- Sanitize data.
  tmpPacket.RSSI = esp_now_info->rx_ctrl->rssi;
  memcpy(tmpPacket.MAC, esp_now_info->src_addr, 6); //- Copy MAC address to queue packet.
  memcpy(&tmpPacket.queuePacket, incomingData, len); //- Only copy up to the "len" to avoid accidently accesing random memory parts.
  
  if(xQueueSend(meshPacket_Queue, &tmpPacket, 0) != pdTRUE) //- Push to queue.
  {
    //- ToDo: Implemented error flag, statistics.
    Serial.printf("[MESH][ERROR]: Queue is FULL. Packet dropped!\n");
  }
}

void meshPacket_processPackets(uint8_t *acceptedDeviceIDs, uint8_t acceptedDeviceCount, uint32_t waitTime_ms)
{
  if(acceptedDeviceIDs == NULL) return;

  //- Update routing table, drop stale routings entries.
  meshPacket_routeAge();

  meshPacketQueue_t localQueuePacket;
  while(xQueueReceive(meshPacket_Queue, &localQueuePacket, pdMS_TO_TICKS(waitTime_ms)) == pdTRUE) //- waitTime_ms to prevent xQueueReceive() hammering in a tight spins (no delay, no blocking).
  {
    meshPacket_t *localPacket = &localQueuePacket.queuePacket;

    //- Drop mesh packet if already seen.
    if(meshPacket_isPacketSeen(localPacket->sourceID, localPacket->uniqueIdentifier)) continue;

    //- Remember mesh packet.
    meshPacket_rememberPacket(localPacket->sourceID, localPacket->uniqueIdentifier);

    #ifdef ENABLE_DEBUG_MESSAGES
    Serial.printf("[MESH][INFO]: Packet received S%02d, D%02d, T%02d, Len%02d, UID%05d, RSSI: %ddBm\n", localPacket->sourceID, localPacket->destinationID, localPacket->packetType, localPacket->payloadLength, localPacket->uniqueIdentifier, localQueuePacket.RSSI);
    #endif

    //- Accept if destinationID matches any in acceptedDeviceIDs OR is broadcast (0xFF).
    volatile bool meshPacket_packetProcessed = false;
    for(uint8_t i = 0; i < acceptedDeviceCount; i++)
    {
      if(localPacket->destinationID == acceptedDeviceIDs[i] || localPacket->destinationID == DEVICE_ID_BROADCAST)
      {
        meshPacket_packetProcessed = true;
        
        //- ACK for me, mark delivered.
        if(localPacket->packetType == PACKET_TYPE_ACKNOWLEDGEMENT)
        {
          meshPacket_markDelivered(localPacket->uniqueIdentifier, localPacket->sourceID);
          break;
        }

        if(meshPacket_handlePacketCallback != NULL)
        {
          meshPacket_handlePacketCallback(localPacket);
          break;
        }
      }
    }
    
    //- Learn reverse route: "to reach S, forward via MAC"
    int index = meshPacket_routeFind(localPacket->sourceID);
    if(index >= 0) 
    {
      routingTable[index].lastSeen = millis(); //- Update timestamp.
      routingTable[index].lastRSSI = localQueuePacket.RSSI; //- Update RSSI.
    }
    else
    {
      meshPacket_routeAdd(localPacket->sourceID, localQueuePacket.MAC, localQueuePacket.RSSI);

      #ifdef ENABLE_DEBUG_MESSAGES
      Serial.printf("[MESH][INFO]: New route to D%02d\n", localPacket->sourceID);
      #endif
    }
    
    //- Packet wasn't meant for me, let's route it.
    if(!meshPacket_packetProcessed)
    { 
      int idx = meshPacket_routeFind(localPacket->destinationID);
      const uint8_t *mac = (idx >= 0) ? routingTable[idx].nextHopMAC : meshPacket_broadcastAddress;

      //- Log details, including MAC.
      #ifdef ENABLE_DEBUG_MESSAGES
      Serial.printf("[MESH][INFO]: Packet S%02d, D%02d, T%02d, L%02d, UID%05d\n", localPacket->sourceID, localPacket->destinationID, localPacket->packetType, localPacket->payloadLength, localPacket->uniqueIdentifier);
      Serial.printf("[MESH][INFO]: Routed to D%02d / MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", (idx >= 0) ? routingTable[idx].destinationID : DEVICE_ID_BROADCAST, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      #endif

      meshPacket_retransmitPacket(localPacket, mac);
    }

    //- Packet was meant for me, let's return ACK.
    if(meshPacket_packetProcessed && localPacket->packetType != PACKET_TYPE_ACKNOWLEDGEMENT)
    {
      #ifdef ENABLE_DEBUG_MESSAGES
      Serial.printf("[MESH][INFO]: Sending ACK: S%02d, D%02d, T%02d UID%05d\n", localPacket->destinationID, localPacket->sourceID, PACKET_TYPE_ACKNOWLEDGEMENT, localPacket->uniqueIdentifier);
      #endif

      //- Send acknowledgement.
      meshPacket_sendMessage(localPacket->destinationID, localPacket->sourceID, PACKET_TYPE_ACKNOWLEDGEMENT, NULL, 0, false, localPacket->uniqueIdentifier);
    }

    //- ToDo: Pridėti atskirą BROADCAST persiuntimą.
  }

  //checkRetransmissions(); //- ToDo: Move to a better place?
}

//====================================== HELPER FUNCTIONS =============================================//
void meshPacket_printRoutingTable() 
{
  Serial.println("\n========================== Routing Table ==========================");
  Serial.println("Idx | DestID |    MAC Address    | LastSeen(ms)| InUse | RSSI (dBm) |");
  Serial.println("----|--------|-------------------|-------------|-------|------------|");

  for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES; i++) 
  {
    if(routingTable[i].inUse) 
    {
      Serial.printf("%3d | %6d | %02X:%02X:%02X:%02X:%02X:%02X | %11lu | %-5s | %12d |\n",
        i,
        routingTable[i].destinationID,
        routingTable[i].nextHopMAC[0], routingTable[i].nextHopMAC[1], routingTable[i].nextHopMAC[2],
        routingTable[i].nextHopMAC[3], routingTable[i].nextHopMAC[4], routingTable[i].nextHopMAC[5],
        millis() - routingTable[i].lastSeen,
        routingTable[i].inUse ? "Yes" : "No",
        routingTable[i].lastRSSI);
    }
  }
  Serial.println("===================================================================\n");
}
