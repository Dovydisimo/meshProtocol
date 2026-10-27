


//========================================= INCLUDES ==============================================//
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_system.h"

#include "meshProtocol.h"


//====================================== PRIVATE VARIABLES =============================================//
uint8_t meshPacket_cacheIndex = 0;
uint16_t meshPacket_messageCounter = 0;
uint32_t meshPacket_deviceLastSeen[MAX_IOT_DEVICES];

QueueHandle_t meshPacket_Queue;
static SemaphoreHandle_t messageMutex = NULL;

uint8_t meshPacket_broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};


meshPacketCache_t meshPacketCache[MESH_PACKET_CACHE_SIZE];
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
  
  meshPacket_messageCounter = (uint16_t)esp_random();      //- Distinct UID range per boot (counter used to restart at 0).
  memset(meshPacketCache, 0xFF, sizeof(meshPacketCache));  //- Sentinel instead of zero-init (0,0) entries.

  meshPacket_Queue = xQueueCreate(MESH_PACKET_QUEUE_LENGTH, sizeof(meshPacketQueue_t));
  if(meshPacket_Queue == NULL)
  {
    return ESP_FAIL;
  }
  
  messageMutex = xSemaphoreCreateMutex();
  if(messageMutex == NULL)
  {
    return ESP_FAIL;
  }
  
  //- NOTE: Register callbacks after queue in case a packet arrives before the queue exists.
  esp_now_register_send_cb(esp_now_send_cb_t(meshPacket_OnDataSent)); //- Register Send callback to get the status of trasnmitted packet.
  esp_now_register_recv_cb(esp_now_recv_cb_t(meshPacket_OnDataRecv)); //- Register callback to get received packet info.

  //- Register broadcast peer. Unicast next hops are registered lazily in meshPacket_nextHop().
  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, meshPacket_broadcastAddress, 6);
  broadcastPeer.channel = wifiChannel;
  broadcastPeer.encrypt = false;
  return esp_now_add_peer(&broadcastPeer);
}

int meshPacket_routeFind(uint8_t destID)
{
  for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES; i++)
  {
    if(routingTable[i].inUse && routingTable[i].destinationID == destID) return i;
  }
  return -1;
}

//- Unregisters ESP-NOW peers that are neither the broadcast address nor the next hop of any active route.
static void meshPacket_prunePeers()
{
  uint8_t stale[MAX_PEERS][6];
  uint8_t staleCount = 0;

  esp_now_peer_info_t peer;
  bool fromHead = true;
  while(staleCount < MAX_PEERS && esp_now_fetch_peer(fromHead, &peer) == ESP_OK)
  {
    fromHead = false;
    if(memcmp(peer.peer_addr, meshPacket_broadcastAddress, 6) == 0) continue; //- Never remove the broadcast peer.

    bool used = false;
    for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES && !used; i++)
    {
      used = routingTable[i].inUse && memcmp(routingTable[i].nextHopMAC, peer.peer_addr, 6) == 0;
    }
    if(!used) memcpy(stale[staleCount++], peer.peer_addr, 6);
  }

  for(uint8_t i = 0; i < staleCount; i++) //- Delete after iterating, so the peer list isn't modified mid-walk.
  {
    esp_now_del_peer(stale[i]);

    #ifdef ENABLE_DEBUG_MESSAGES
    meshPacket_sendTerminalMessage("[MESH][INFO]: Peer %02X:%02X:%02X:%02X:%02X:%02X removed\n", stale[i][0], stale[i][1], stale[i][2], stale[i][3], stale[i][4], stale[i][5]);
    #endif
  }
}

//- Returns the MAC to transmit to for destID: the route's next hop (registered as an ESP-NOW peer on first use), or broadcast when there is no route or the peer table is full.
static const uint8_t *meshPacket_nextHop(uint8_t destID, int *routeIdx = NULL)
{
  int idx = meshPacket_routeFind(destID);
  if(routeIdx != NULL) *routeIdx = idx;
  if(idx < 0) return meshPacket_broadcastAddress;

  const uint8_t *mac = routingTable[idx].nextHopMAC;
  if(!esp_now_is_peer_exist(mac))
  {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0; //- 0 = use the current Wi-Fi channel.
    peerInfo.encrypt = false;
    if(esp_now_add_peer(&peerInfo) != ESP_OK)
    {
      meshPacket_prunePeers(); //- Peer table is probably full: free slots held by old peers and retry once.
      if(esp_now_add_peer(&peerInfo) != ESP_OK) return meshPacket_broadcastAddress; //- Still no room: fall back to flooding.
    }
  }
  return mac;
}

//- Cost of ONE link: a fixed hop cost plus a penalty that grows linearly as the link RSSI drops from GOOD to POOR. The path cost is the sum of these, carried in meshPacket_t.pathCost.
static uint8_t meshPacket_linkCost(int8_t rssi)
{
  int16_t penalty = ((int16_t)MESH_LINK_RSSI_GOOD_DBM - rssi) * MESH_LINK_MAX_PENALTY / (MESH_LINK_RSSI_GOOD_DBM - MESH_LINK_RSSI_POOR_DBM);
  if(penalty < 0) penalty = 0;
  if(penalty > MESH_LINK_MAX_PENALTY) penalty = MESH_LINK_MAX_PENALTY;
  return (uint8_t)(MESH_PACKET_COST_PER_HOP + penalty);
}

esp_err_t meshPacket_routeAdd(uint8_t destID, const uint8_t *nextHopMAC, int8_t RSSI, uint8_t hops, uint8_t cost, bool allowCreate)
{
  if(destID == DEVICE_ID_BROADCAST || destID == DEVICE_ID_UNCONFIGURED) return ESP_ERR_INVALID_ARG; //- Never learn routes to reserved IDs (would hijack broadcast/beacon sends, which look up routes by destinationID).

  int idx = meshPacket_routeFind(destID);
  if(idx < 0 && !allowCreate) return ESP_OK; //- Duplicate copies may only refresh/replace an existing route, never create one (an echo of our own forward could create a loop).
  bool newRoute = (idx < 0); //- Result is -1 if no route exists. Keep this value as it can change later (free slot can be assigned).
  int8_t storedRSSI = RSSI;
  uint8_t storedCost = cost;
  if(idx >= 0) //- Route exists: refresh it, or replace it only if the new path is clearly cheaper.
  {
    bool sameNextHop = (memcmp(routingTable[idx].nextHopMAC, nextHopMAC, 6) == 0);
    uint8_t currentCost = routingTable[idx].cost;
    bool betterPath = ((uint16_t)cost + MESH_PACKET_ROUTE_COST_HYSTERESIS < currentCost);
    if(!sameNextHop && !betterPath) return ESP_OK; //- Keep current route. Don't refresh lastSeen, so a dead best route can still expire.

    if(sameNextHop) //- Smooth RSSI and cost on the same link, so a single fade doesn't swing the cost.
    {
      storedRSSI = (int8_t)((routingTable[idx].lastRSSI * 3 + RSSI) / 4);
      storedCost = (uint8_t)(((uint16_t)currentCost * 3 + cost) / 4);
    }

    #ifdef ENABLE_DEBUG_MESSAGES
    if(!sameNextHop) meshPacket_sendTerminalMessage("[MESH][INFO]: Route to D%02d changed (%d hops, cost %u -> %d hops, cost %u)\n", destID, routingTable[idx].hops, currentCost, hops, cost);
    #endif
  }
  else //- No existing route, try to allocate a new slot.
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
  
  if(idx < 0) return ESP_FAIL; //- Return FAIL when no route is found or table is full.

  //- Otherwise assign, refresh or replace the route in idx slot.
  routingTable[idx].inUse = true;
  routingTable[idx].destinationID = destID;
  routingTable[idx].lastSeen = millis();
  routingTable[idx].lastRSSI = storedRSSI;
  routingTable[idx].hops = hops;
  routingTable[idx].cost = storedCost;
  memcpy(routingTable[idx].nextHopMAC, nextHopMAC, 6);

  #ifdef ENABLE_DEBUG_MESSAGES
  if(newRoute) meshPacket_sendTerminalMessage("[MESH][INFO]: New route to D%02d (%d hops)\n", destID, hops);
  #endif

  return ESP_OK;
}

void meshPacket_routeAge()
{
  uint32_t now = millis();
  bool anyExpired = false;
  for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES; i++)
  {
    if(routingTable[i].inUse && (now - routingTable[i].lastSeen > MESH_PACKET_NODE_EXPIRE_TIME_MS))
    {
      routingTable[i].inUse = false; //- Mark as expired.
      anyExpired = true;

      #ifdef ENABLE_DEBUG_MESSAGES
      meshPacket_sendTerminalMessage("[MESH][INFO]: Route for D%02d expired\n", routingTable[i].destinationID);
      #endif
    }
  }

  if(anyExpired) meshPacket_prunePeers(); //- Drop ESP-NOW peers no remaining route uses.
}

void meshPacket_rememberPacket(uint8_t sourceID, uint16_t uniqueIdentifier, uint8_t TTL)
{
  meshPacketCache[meshPacket_cacheIndex].sourceID = sourceID;
  meshPacketCache[meshPacket_cacheIndex].uniqueIdentifier = uniqueIdentifier;
  meshPacketCache[meshPacket_cacheIndex].TTL = TTL; //- TTL of the first copy, used to judge later duplicates.
  //meshPacketCache[meshPacket_cacheIndex].lastSeen = millis();
  meshPacket_cacheIndex = (meshPacket_cacheIndex + 1) % MESH_PACKET_CACHE_SIZE;

  if(sourceID < MAX_IOT_DEVICES)
  {
    meshPacket_deviceLastSeen[sourceID] = millis();
  }
}

//- Returns the seen-cache index of the packet, or -1 if it was not seen.
int meshPacket_findSeenPacket(uint8_t sourceID, uint16_t uniqueIdentifier)
{
  for(uint8_t i = 0; i < MESH_PACKET_CACHE_SIZE; i++)
  {
    if(meshPacketCache[i].sourceID == sourceID && meshPacketCache[i].uniqueIdentifier == uniqueIdentifier) return i;
  }
  return -1;
}

bool meshPacket_isPacketSeen(uint8_t sourceID, uint16_t uniqueIdentifier)
{
  return meshPacket_findSeenPacket(sourceID, uniqueIdentifier) >= 0;
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
      meshPacket_sendTerminalMessage("[MESH][INFO]: ACK received from S%02u\n", fromNode);
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
      meshPacket_sendTerminalMessage("Retrying message %u to node %u (attempt %u)\n", pending[i].uniqueID, pending[i].destID, pending[i].retries + 1);

      //meshPacket_retransmitPacket(meshPacket_t *receivedPacket)
      esp_now_send(meshPacket_broadcastAddress, (uint8_t *)&pending[i].packet, pending[i].packet.payloadLength + MESH_PACKET_HEADER_LENGTH);

      pending[i].retries++;
      pending[i].lastSend = now;
    }
  }
}*/

esp_err_t meshPacket_sendBeacon(uint8_t sourceID)
{
  return meshPacket_sendMessage(sourceID, DEVICE_ID_BROADCAST, PACKET_TYPE_BEACON, (uint8_t *)"BEACON", 6, false, -1);
}

void meshPacket_retransmitPacket(meshPacket_t *receivedPacket, const uint8_t *MAC)
{
  if(receivedPacket->TTL > 0) //- Forward only if TTL > 0.
  {
    receivedPacket->TTL--; //- Decrement TTL before sending.
    vTaskDelay(pdMS_TO_TICKS((esp_random() % 5) + 1)); //- Slight delay to avoid saturating network.
    esp_now_send(MAC, (uint8_t *)receivedPacket, receivedPacket->payloadLength + MESH_PACKET_HEADER_LENGTH);
  }
}

esp_err_t meshPacket_sendMessage(uint8_t sourceID, uint8_t destinationID, uint8_t packetType, const uint8_t *payload, uint8_t payloadLength, bool loopback, int32_t forceUID)
{
  if(payload == NULL && payloadLength > 0) return ESP_ERR_INVALID_ARG; //- NULL only allowed if length == 0.
  if(payloadLength > MAXIMUM_PACKET_LENGTH - MESH_PACKET_HEADER_LENGTH) return ESP_ERR_INVALID_ARG; //- Return error.

  meshPacket_t sendPacket;
  memset(&sendPacket, 0, MESH_PACKET_HEADER_LENGTH); //- pathCost = 0 at the source, reserved bytes zeroed.
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
  meshPacket_rememberPacket(sendPacket.sourceID, sendPacket.uniqueIdentifier, sendPacket.TTL);

  if(packetType != PACKET_TYPE_ACKNOWLEDGEMENT && destinationID != DEVICE_ID_BROADCAST) //- Broadcasts can't be tracked per destination.
  {
    meshPacket_addPendingAck(sendPacket.uniqueIdentifier, sendPacket.destinationID, &sendPacket);
  }

  int idx;
  const uint8_t *mac = meshPacket_nextHop(destinationID, &idx);

  //- Log details, including MAC.
  #ifdef ENABLE_DEBUG_MESSAGES
  meshPacket_sendTerminalMessage("[MESH][INFO]: Sending packet S%02d, D%02d, T%02d, L%02d, UID%05d\n", sourceID, destinationID, packetType, payloadLength, sendPacket.uniqueIdentifier);
  meshPacket_sendTerminalMessage("[MESH][INFO]: First hop D%02d / MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", (idx >= 0) ? routingTable[idx].destinationID : DEVICE_ID_BROADCAST, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  #endif

  return esp_now_send(mac, (const uint8_t*)&sendPacket, payloadLength + MESH_PACKET_HEADER_LENGTH);
}

void meshPacket_OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  /*for(uint8_t i = 0; i < MAX_IOT_DEVICES; i++)
  {
    if(deviceTelemetry[i].device_MACaddr[0] == mac_addr[0] && deviceTelemetry[i].device_MACaddr[1] == mac_addr[1] && deviceTelemetry[i].device_MACaddr[2] == mac_addr[2] && deviceTelemetry[i].device_MACaddr[3] == mac_addr[3] && deviceTelemetry[i].device_MACaddr[4] == mac_addr[4] && deviceTelemetry[i].device_MACaddr[5] == mac_addr[5])
    {
      meshPacket_sendTerminalMessage("[ESP-NOW][INFO]: %s control packet sent", deviceTelemetry[i].deviceName);
      meshPacket_sendTerminalMessage(status == ESP_NOW_SEND_SUCCESS ? " SUCCESFULLY!" : " UNSUCCESFULLY!");

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
    meshPacket_sendTerminalMessage("[MESH][ERROR]: Queue is FULL. Packet dropped!\n");
  }
}

void meshPacket_processPackets(uint8_t *acceptedDeviceIDs, uint8_t acceptedDeviceCount, uint32_t waitTime_ms)
{
  if(acceptedDeviceIDs == NULL) return;

  //- Update routing table, drop stale routing entries.
  meshPacket_routeAge();

  meshPacketQueue_t localQueuePacket;
  while(xQueueReceive(meshPacket_Queue, &localQueuePacket, pdMS_TO_TICKS(waitTime_ms)) == pdTRUE) //- waitTime_ms to prevent xQueueReceive() hammering in a tight spins (no delay, no blocking).
  {
    meshPacket_t *receivedPacket = &localQueuePacket.queuePacket;

    //- Learn reverse route: "to reach S, forward via MAC". NOTE: Runs for EVERY copy (also duplicates), before the packet is retransmitted (which decrements TTL; a direct neighbour sees TTL == HOP_LIMIT -> 1 hop).
    uint8_t hops = (receivedPacket->TTL <= MESH_PACKET_HOP_LIMIT) ? (MESH_PACKET_HOP_LIMIT - receivedPacket->TTL + 1) : 1; //- Clamp bogus TTL (ex., TTL is higher than MESH_PACKET_HOP_LIMIT).
    //- Cost of the path to the source, ending on the link this copy arrived on. Carried cost is clamped to what is possible for this hop count (garbage / spoofed / older FW).
    uint16_t minCost = (uint16_t)(hops - 1) * MESH_PACKET_COST_PER_HOP; //- Every earlier hop costs at least this much.
    uint16_t maxCost = (uint16_t)(hops - 1) * MESH_LINK_MAX_COST;       //- ...and at most this much.
    uint16_t carriedCost = receivedPacket->pathCost;
    if(carriedCost < minCost) carriedCost = minCost; //- A forwarder did not add its link (older FW).
    if(carriedCost > maxCost) carriedCost = minCost; //- Impossible value: fall back to the hop count.
    uint16_t totalCost = carriedCost + meshPacket_linkCost(localQueuePacket.RSSI);
    uint8_t pathCost = (totalCost > 255) ? 255 : (uint8_t)totalCost;

    bool sourceIsMe = false; //- Never learn a route to ourselves (e.g. a stale echo of our own packet after the seen-cache wrapped).
    for(uint8_t i = 0; i < acceptedDeviceCount; i++) if(receivedPacket->sourceID == acceptedDeviceIDs[i]) sourceIsMe = true;

    int seenIdx = meshPacket_findSeenPacket(receivedPacket->sourceID, receivedPacket->uniqueIdentifier);
    bool duplicate = (seenIdx >= 0);

    if(!sourceIsMe)
    {
      if(!duplicate) meshPacket_routeAdd(receivedPacket->sourceID, localQueuePacket.MAC, localQueuePacket.RSSI, hops, pathCost, true);
      else if(receivedPacket->TTL + 1 >= meshPacketCache[seenIdx].TTL) meshPacket_routeAdd(receivedPacket->sourceID, localQueuePacket.MAC, localQueuePacket.RSSI, hops, pathCost, false); //- Only copies at most 1 hop longer than the first copy count; this rules out our own forwarded packet echoed back by a neighbour (TTL - 2).
    }
    if(duplicate) continue; //- Everything below (callback, ACK, forwarding) still runs once per packet.

    //- Remember mesh packet.
    meshPacket_rememberPacket(receivedPacket->sourceID, receivedPacket->uniqueIdentifier, receivedPacket->TTL);

    #ifdef ENABLE_DEBUG_MESSAGES
    meshPacket_sendTerminalMessage("[MESH][INFO]: Packet received S%02d, D%02d, T%02d, Len%02d, UID%05d, TTL%d, RSSI: %ddBm, from %02X:%02X:%02X:%02X:%02X:%02X\n", receivedPacket->sourceID, receivedPacket->destinationID, receivedPacket->packetType, receivedPacket->payloadLength, receivedPacket->uniqueIdentifier, receivedPacket->TTL, localQueuePacket.RSSI, localQueuePacket.MAC[0], localQueuePacket.MAC[1], localQueuePacket.MAC[2], localQueuePacket.MAC[3], localQueuePacket.MAC[4], localQueuePacket.MAC[5]);
    #endif

    //- Accept if destinationID matches any in acceptedDeviceIDs OR is broadcast.
    volatile bool meshPacket_packetProcessed = false;
    for(uint8_t i = 0; i < acceptedDeviceCount; i++)
    {
      if(receivedPacket->destinationID == acceptedDeviceIDs[i] || receivedPacket->destinationID == DEVICE_ID_BROADCAST)
      {
        meshPacket_packetProcessed = true;
        
        //- ACK for me, mark delivered.
        if(receivedPacket->packetType == PACKET_TYPE_ACKNOWLEDGEMENT)
        {
          meshPacket_markDelivered(receivedPacket->uniqueIdentifier, receivedPacket->sourceID);
          break;
        }

        if(meshPacket_handlePacketCallback != NULL)
        {
          meshPacket_handlePacketCallback(receivedPacket);
          break;
        }
      }
    }
    
	//- Packet was meant for me, let's return ACK (beacons are ACKed for route discovery, other broadcasts are not).
    bool needsAck = receivedPacket->packetType == PACKET_TYPE_BEACON || receivedPacket->destinationID != DEVICE_ID_BROADCAST;
	if(meshPacket_packetProcessed && receivedPacket->packetType != PACKET_TYPE_ACKNOWLEDGEMENT && needsAck && acceptedDeviceIDs[0] != DEVICE_ID_UNCONFIGURED)
	{
      //- LIMITATION: Beacon responses do not support multiple acceptedDeviceIDs[0] and only use first entry as sourceID.
	  uint8_t ackSourceID = receivedPacket->packetType == PACKET_TYPE_BEACON ? acceptedDeviceIDs[0] : receivedPacket->destinationID;

      #ifdef ENABLE_DEBUG_MESSAGES
      meshPacket_sendTerminalMessage("[MESH][INFO]: Sending ACK: S%02d, D%02d, T%02d UID%05d\n", ackSourceID, receivedPacket->sourceID, PACKET_TYPE_ACKNOWLEDGEMENT, receivedPacket->uniqueIdentifier);
      #endif

      //- Send acknowledgement.
      meshPacket_sendMessage(ackSourceID, receivedPacket->sourceID, PACKET_TYPE_ACKNOWLEDGEMENT, NULL, 0, false, receivedPacket->uniqueIdentifier);
    }
	
    //- Packet wasn't meant for me, or is a broadcast: let's route/flood it.
	if(!meshPacket_packetProcessed || receivedPacket->destinationID == DEVICE_ID_BROADCAST)
    { 
      int idx;
      const uint8_t *mac = meshPacket_nextHop(receivedPacket->destinationID, &idx);

      //- Log details, including MAC.
      #ifdef ENABLE_DEBUG_MESSAGES
      meshPacket_sendTerminalMessage("[MESH][INFO]: Packet S%02d, D%02d, T%02d, L%02d, UID%05d\n", receivedPacket->sourceID, receivedPacket->destinationID, receivedPacket->packetType, receivedPacket->payloadLength, receivedPacket->uniqueIdentifier);
      meshPacket_sendTerminalMessage("[MESH][INFO]: Routed to D%02d / MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", (idx >= 0) ? routingTable[idx].destinationID : DEVICE_ID_BROADCAST, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      #endif

      receivedPacket->pathCost = pathCost; //- Next node adds its own incoming link on top of this.
      meshPacket_retransmitPacket(receivedPacket, mac);
    }
  }

  //checkRetransmissions(); //- ToDo: Move to a better place?
}

void meshPacket_sendTerminalMessage(const char *format, ...)
{
  char buffer[128];

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if(xSemaphoreTake(messageMutex, pdMS_TO_TICKS(10)) == pdTRUE) //- portMAX_DELAY
  {
    meshPacket_messageHandler(buffer);
    xSemaphoreGive(messageMutex);
  }
}

__attribute__((weak)) void meshPacket_messageHandler(const char *message)
{
//	Serial.print(message);
}

//====================================== HELPER FUNCTIONS =============================================//
void meshPacket_printRoutingTable() 
{
  meshPacket_sendTerminalMessage("\n================================== Routing Table ==================================\n");
  meshPacket_sendTerminalMessage("Idx | DestID |    MAC Address    | LastSeen(ms)| InUse | RSSI (dBm) | Hops | Cost |\n");
  meshPacket_sendTerminalMessage("----|--------|-------------------|-------------|-------|------------|------|------|\n");

  for(uint8_t i = 0; i < MESH_PACKET_MAX_ROUTES; i++) 
  {
    if(routingTable[i].inUse) 
    {
      meshPacket_sendTerminalMessage("%3d | %6d | %02X:%02X:%02X:%02X:%02X:%02X | %11lu | %-5s | %10d | %4d | %4u |\n",
        i,
        routingTable[i].destinationID,
        routingTable[i].nextHopMAC[0], routingTable[i].nextHopMAC[1], routingTable[i].nextHopMAC[2],
        routingTable[i].nextHopMAC[3], routingTable[i].nextHopMAC[4], routingTable[i].nextHopMAC[5],
        millis() - routingTable[i].lastSeen,
        routingTable[i].inUse ? "Yes" : "No",
        routingTable[i].lastRSSI,
        routingTable[i].hops,
        routingTable[i].cost);
    }
  }
  meshPacket_sendTerminalMessage("==================================================================================\n");
}