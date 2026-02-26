= ESP-NOW Mesh Protocol Library
Dovydas Bružas <Dovydisimo@gmail.com>
:toc:
:icons:

Lightweight and fast ESP-NOW mesh networking library for ESP32 devices.
Supports multi-hop routing, acknowledgements, node discovery, and packet
validation with minimal overhead.

== Features

* Multi-hop routing via ESP-NOW
* Automatic route aging and management
* ACK-based delivery reliability
* Duplicate packet suppression
* Peer table and routing table management
* Packet queueing (FreeRTOS)
* User-defined packet handler callback
* Very low overhead – designed for IoT nodes

== Installation

1. Download or clone this repository
2. Place it in your Arduino libraries directory:

----
Documents/Arduino/libraries/ESPNowMeshProtocol/
----

3. Restart Arduino IDE

== Getting Started

=== 1. Initialize the Mesh

Call in `setup()`:

```cpp
meshPacket_init(3);   // Set WiFi channel for ESP-NOW
=== 2. Process Packets

Call in loop():

cpp
Copy code
meshPacket_processPackets((uint8_t[]){LOCAL_DEVICE_ID}, 1, 50);
acceptedDeviceIDs filters which device IDs you want to receive.

=== 3. Handle Incoming Packets

Override the weak callback:

cpp
Copy code
void meshPacket_handlePacketCallback(meshPacket_t *packet) {
    if (packet->packetType == PACKET_TYPE_TELEMETRY) {
        Serial.println("Telemetry received!");
    }
}
=== 4. Sending a Message

cpp
Copy code
uint8_t payload[] = { 1, 2, 3, 4 };
meshPacket_sendMessage(LOCAL_DEVICE_ID, DEVICE_ID_MPPT_CONTROLLER, PACKET_TYPE_CONTROL, payload, sizeof(payload));
== API Reference

=== Initialization

meshPacket_init(channel)

=== Peer Management

meshProtocol_addPeer(mac, nodeID, channel)

meshProtocol_removePeer(mac)

=== Routing

meshPacket_routeFind(destID)

meshPacket_routeAdd(destID, nextHopMAC, RSSI)

meshPacket_routeAge()

meshPacket_printRoutingTable()

=== Messaging

meshPacket_sendMessage(...)

meshPacket_processPackets(...)

=== Callbacks

meshPacket_handlePacketCallback(packet) (weak, override in sketch)

meshPacket_OnDataRecv(...)

meshPacket_OnDataSent(...)

== License

Public domain / Beerware (to be finalized).

If you find this useful, buy the author a beer 🍺 :)

== TODO

Beacon support (RREQ/RREP)

Route discovery improvements

Benchmark statistics

Thread-safety improvements (semaphores)

Finish retransmission logic
