/*
                            meshProtocol.h - Library for meshing ESP32 devices via ESP-NOW.
                                         Created by Dovydas Bružas, 2025 July 31.
                                            Released into the public domain.


                                          --- IMPLEMENTATION INSTRUCTIONS ---
      1. Place meshPacket_init(uint8_t wifiChannel) under the main() or setup() function. Set the channel to match a Wi-Fi channel.
      2. Place void meshPacket_processPackets(uint8_t *acceptedDeviceIDs, uint8_t acceptedDeviceCount) under loop() or a FreeRTOS task. 
          2.1. NOTE: The pointer acceptedDeviceIDs sets the accepted device IDs, so packets can be filtered accordingly. Use ((uint8_t[]){LOCAL_DEVICE_ID}, 1) to accept only native device.
          2.2. Note: FreeRTOS Queue API is used for local packet storage.
      3. Place void meshPacket_handlePacketCallback(meshPacket_t *localPacket) in your main.c. By default the function is declared as week.
          3.1. Use localPacket->packetType to check the packet type and handle it accordingly.
      4. Profit


        --- LIMITATIONS ---
  1. CAUTION: Not confirmed to be thread-safe. 
  2. 
  3. 


        --- TODO ---
  1. Perkelti biblioteką į normalia lokaciją su .c ir .h.
  2. Implementuoti beacon'ą kaimynui atradimui ir geresniam route discovery. RREQ / RREP 
  3. Peržiūrėti resursus, kuriuos dalinasi task'ai ir sudėti semhaphoras.
  5. Pabaigti implementaciją → checkRetransmissions().
  6. Pabaigti implementaciją → meshPacket_sendBeacon.
  7. Add beerware license descriptor.
  8. Pridėti benchmark statistikas.
  9. Pridėti visų peers spausdinimo funkciją.
  10. 
*/

#ifndef meshProtocol_h
#define meshProtocol_h

//========================================= INCLUDES ==============================================//
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <esp_now.h>


//========================================= DEFINES ==============================================//
#define ENABLE_DEBUG_MESSAGES                    //- Great for debugging, comment for production code.

#define MAX_PEERS                         20     //- Limited by ESP-NOW.
#define MAXIMUM_PACKET_LENGTH	          250    //- Limited by ESP-NOW maximum packet size.

#define MESH_PACKET_MAX_ROUTES            30     //- Maximum number of routes a device can hold. Maximum is 255.
#define MESH_PACKET_CACHE_SIZE            25     //- Number of mesh packets system remembers. Maximum is 255.
#define MESH_PACKET_HEADER_LENGTH         11     //- 
#define MESH_PACKET_HOP_LIMIT             5      //- 5-hop limit. Maximum is 255.
#define MESH_PACKET_QUEUE_LENGTH          36     //- Queue length to store meshPackets.
#define MESH_PACKET_PENDING_ACKS          20     //- Maximum number of ACKs a device can hold at the same time. Maximum is 255.
#define MESH_PACKET_NODE_EXPIRE_TIME_MS   900000 //- Timeout value for route

#define MAX_IOT_DEVICES                   	128      //- Maximum is 255.
#define DEVICE_ID_INTERNET_GATEWAY        	0
#define DEVICE_ID_MPPT_CONTROLLER         	1
#define DEVICE_ID_WATER_BOILER            	2
#define DEVICE_ID_SAUNA_CONTROLLER        	3
#define DEVICE_ID_MPPT_CONTROLLER_2       	4
#define DEVICE_ID_CLIMATE_LOGGER_0			5
#define DEVICE_ID_CLIMATE_LOGGER_1			6
//#define DEVICE_ID_WATER_BOILER_TEST    	  11	 //- TODO: IŠTRINTI PO TESTU.
//----------------- SERVICE DEVICE IDs -----------------//
#define DEVICE_ID_DATABASE                250
#define DEVICE_ID_INVALID                 253
#define DEVICE_ID_BROADCAST               254

#define PACKET_TYPE_TELEMETRY             0
#define PACKET_TYPE_CONTROL               1
#define PACKET_TYPE_NOTIFICATION          2
#define PACKET_TYPE_ACKNOWLEDGEMENT       100
#define PACKET_TYPE_BEACON                101


//====================================== STRUCTURE VARIABLES =============================================//
struct __attribute__((packed)) meshPacket_t
{
  uint8_t sourceID;
  uint8_t destinationID;
  uint8_t packetType;
  uint8_t payloadLength;
  uint8_t TTL;
  uint16_t uniqueIdentifier;
  uint32_t reserved;
  char payload[MAXIMUM_PACKET_LENGTH - MESH_PACKET_HEADER_LENGTH];
};

struct __attribute__((packed)) meshPacketQueue_t
{
  int8_t RSSI;
  uint8_t MAC[6];  
  meshPacket_t queuePacket;
};

struct __attribute__((packed)) meshPacketCache_t
{
  uint8_t sourceID;
  uint16_t uniqueIdentifier;
  //uint32_t lastSeen;
};

struct __attribute__((packed)) knownPeers_t
{
  bool inUse;
  uint8_t nodeID;
  uint8_t MAC[6];

};

struct __attribute__((packed)) routingTable_t
{
    bool inUse;                 //- Marks the slot as used/unused.
    uint8_t destinationID;      //- Final node we want to reach.
    uint8_t nextHopMAC[6];      //- MAC of the next hop.
    uint32_t lastSeen;          //- millis() timestamp for aging.
    int8_t lastRSSI;

};

struct __attribute__((packed)) PendingAck_t
{
  uint16_t uniqueID;
  uint8_t destID;
  uint8_t retries;
  unsigned long lastSend;
  meshPacket_t packet;
};


//========================================= FUNCTION PROTOTYPES ==============================================//
esp_err_t meshPacket_init(uint8_t wifiChannel);
esp_err_t meshProtocol_addPeer(const uint8_t *mac, uint8_t nodeID, uint8_t wifiChannel);
esp_err_t meshProtocol_removePeer(uint8_t *mac);
int meshPacket_routeFind(uint8_t destID);
esp_err_t meshPacket_routeAdd(uint8_t destID, const uint8_t *nextHopMAC, int8_t RSSI); //- ToDo: Fix limitation of "want last-seen route wins".
void meshPacket_routeAge();
void meshPacket_rememberPacket(uint8_t sourceID, uint16_t uniqueIdentifier);
bool meshPacket_isPacketSeen(uint8_t sourceID, uint16_t uniqueIdentifier);
uint8_t meshPacket_getActiveDeviceCount(uint32_t lastSeenDeviceThreshold_ms);
void meshPacket_markDelivered(uint16_t uniqueID, uint8_t fromNode);
void meshPacket_addPendingAck(uint16_t uniqueID, uint8_t destID, meshPacket_t *packet);
void meshPacket_retransmitPacket(meshPacket_t *localPacket, const uint8_t *MAC);
esp_err_t meshPacket_sendMessage(uint8_t sourceID, uint8_t destinationID, uint8_t packetType, const uint8_t *payload, uint8_t payloadLength, bool loopback = false, int32_t forceUID = -1);
void meshPacket_processPackets(uint8_t *acceptedDeviceIDs, uint8_t acceptedDeviceCount, uint32_t waitTime_ms);
void meshPacket_printRoutingTable();

void meshPacket_handlePacketCallback(meshPacket_t *localPacket) __attribute__((weak));
void meshPacket_OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len);
void meshPacket_OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);




//========================================= VALIDATION CHECKS ==============================================//
static_assert(MESH_PACKET_HEADER_LENGTH == offsetof(meshPacket_t, payload), "ERROR: meshPacket_t header length mismatch!");




//====================================== CHANGE LOG =============================================//
/*
                  ----  2025-08-04  ----
                      --- v1.1 ---
      1. FEATURE: function "meshPacket_retransmitPacket" introduced to unify packet retransmission logic. 
      2. FIX: function "meshPacket_init" added to set & fix "NULL" address broadcast issue.
      3. FEATURE: meshPacketCache now includes "lastSeen" which stores local millis() field.
      4. FEATURE: Function "meshPacket_getActiveDeviceCount" moved in from internet gateway FW. 
      5. 

                  ----  2025-08-09  ----
                      --- v1.2 ---
      1. FIX: Added local variable "meshPacket_t sendPacket" in meshPacket_sendMessage() to prevent resource sharing and resolve crash seen on the MPPT controller.
      2. FIX: Corruption of retransmited packe fixed. Issue (uint8_t*)&meshPacket was actually the address of the pointer, not the address of the data.
      3. FIX: Rare, hidden edge case fix for loopback packet tranmission. 
      4. 

                  --- 2025-08-11  ---
                     --- v1.3 ---
      1. CHORE: Shared "#define" like PACKET_TYPES, DEVICE_ID moved to this library.
      2. MISC: Delay for retransmitted packet reduced to 1-5 ms and right now commented for testing.
      3. FEATURE: Functions meshPacket_OnDataRecv, meshPacket_processPackets, meshPacket_handlePacketCallback created to move as much functionality to library for easier implementation.
      4. CHORE: Function meshPacket_init modified to return esp_err_t strucutre to be checked.
      5. FEATURE: Function meshpacket_sendMessage now does bounds check on payloadLength and checks if *payload pointer is not NULL.
      6. FIX: Updated meshPacket_getActiveDeviceCount to rely on a separate millis()-based counter instead of meshPacketCache, fixing issues with device count accuracy caused by buffer overwrites.
      7. FEATURE: Thread-safe FreeRTOS queue added for packet queueing. Queue length is controlled by #define MESH_PACKET_QUEUE_LENGTH.
      8. CHORE: packetLenght, uniqueIndentifier typos fixed.
      9. CHORE: meshPacket_messageCounter now uses __atomic_fetch_add().
      10. 

                  --- 2025-08-17  ---
                     --- v1.4 ---
      1. FEATURE: while loop is added for xQueueReceive to process all queue at once instead one message at the time.
      2. FIX: Wait time is added for xQueueReceive() to fix issues in a tight spin (no delay, no blocking) loops causing no messages to be added to queue.
      3. FIX: xHigherPriorityTaskWoken was not referenced in xQueueSendFromISR().
      4. FEATURE: meshPacket_processPackets now accept queue wait time as an argument.
      5. CHORE: Old commented code removed as new one survived "the test of time".
      6. 

                  --- 2025-08-17  ---
                     --- v1.4 ---
      1. CHORE: Out of bounds check is added for meshPacket_deviceLastSeen variable when packets are remembered. Currently, it fails silentely (still needs updating).
      2. FIX: Logic mistake fixed in meshPacket_processPackets function where even processed packets were retransmitted. 
      3. FEATURE: Experimental ACK is implemented (application level). No retransmission logic yet.
      4. 

                  --- 2025-09-05  ---
                    --- v1.5 ---
      1. FEATURE: meshPacket_OnDataRecv() function modified to use new ESP-NOW API. It allows to extract more information like RSSI.
      2. CHORE: ESP-NOW initilisation moved inside meshPacket_init() function. All ESP-NOW callbacks moved to library.
      3. FEATURE: Complete overhaul of mesh algorithm. Routing table, device pairing, full ACK logic is implemented. Broadcast is now used as fallback.
      4. CHORE: xQueueSendFromISR is replaced with xQueueSend as I didn't realize OnDataRecv isn't a true ISR but runs from WiFi task. 
      5. FEATURE: meshPacket_sendMessage() now logs message parameters and next hop ID & MAC (if enabled).
      6. FEATURE: meshPacket_sendMessage() function updated to include arbiratary uniqueIdentified as an argument. This is for usefull for sending ACKs.
      7. FEATURE: meshPacket_sendMessage() now accepts *payload = NULL and payloadLength = 0. Feature to be used for ACK, BEACON, PING functionalities.
      8. CHORE: meshPacket_t packetLength renamed to payloadLength for consistency and clarity.
      9. CHORE: Library implementation documentation updated.
      10. FEATURE: #define ENABLE_DEBUG_MESSAGES is implemented to enable/disable debug messages.
      11. FEATURE: Last received RSSI is now stored in routing table.
      12. CHORES: Chores around code comments and other miscellaneous stuff.
      13. 
	  
	             --- 2025-11-18  ---
                    --- v1.6 ---
	  1. CHORE: library migrated from .ino file to proper library folder (.h, .cpp). No functional changes.
	  2. 
*/


#endif