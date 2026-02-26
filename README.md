# ESP-NOW Mesh Protocol Library

### TLDR;
Lightweight and fast ESP-NOW (and more) mesh networking library for ESP32 devices.  
Supports multi-hop routing, acknowledgements, node discovery, and packet validation with minimal overhead.

### Overview
This library was developed for a specific use case. It's not revolutionary or world-changing; it simply addresses a problem that has already been solved many times by libraries like ESP-MESH, Meshstastic, and others. It does so for a particular scenario that I needed, and hopefully others might find it useful as well. I keep mentioning specific case, so let's brake it down. 

In a large, remote house where tinkerers live, managing multiple custom IoT projects is just part of everyday life. Ensuring that all devices can communicate with each other without Wi-Fi makes the idea of a mesh network very sexy. The main challenge is to keep it simple (KISS - Keep It Simple, Stupid) while supporting various physical communication interfaces (such as ESP-NOW, radio modules).

This boils down to the following requirements:

* Small footprint: The library is ~600 lines of code (it could be smaller), with a protocol overhead of just 11 bytes (also potentially reducible).
* Portability: It should require less than 1% of code changes, mainly just modifying the print() functions.
* Multi-interface support: It must support multiple communication interfaces. (user-defined `meshPacket_OnDataRecv` and `meshPacket_OnDataSent` functions).

---

## Table of Contents
- [Features](#features)
- [How Does It Work](#How-Does-It-Work)
- [Installation](#installation)
- [Getting Started](#getting-started)
  - [Initialize the Mesh](#1-initialize-the-mesh)
  - [Process Packets](#2-process-packets)
  - [Handle Incoming Packets](#3-handle-incoming-packets)
  - [Sending a Message](#4-sending-a-message)
- [License & Author](#license--author)
- [TODO](#todo)
---

## Features
- Multi-hop routing via ESP-NOW
- Automatic route aging and management
- ACK-based delivery reliability
- Duplicate packet suppression
- Peer table and routing table management
- Packet queueing (FreeRTOS)
- User-defined packet handler callback
- Very low overhead – designed for IoT nodes
---

## How Does It Work?
ToDo

---

## Installation
1. Download or clone this repository
2. Place it in your Arduino libraries directory:

```
Documents/Arduino/libraries/meshProtocol/
```

3. Restart Arduino IDE
---

## Getting Started

### 1. Initialize the Mesh

Call in `setup()`:

```cpp
meshPacket_init(uint8_t wifiChannel); //- Set the channel to match a Wi-Fi channel (default is 1).
```

---

### 2. Process Packets

Call in `loop()` or a FreeRTOS task:

```cpp
void meshPacket_processPackets(uint8_t *acceptedDeviceIDs, uint8_t acceptedDeviceCount, uint32_t waitTime_ms);
```

`acceptedDeviceIDs` the pointer sets the accepted device IDs, so packets can be filtered accordingly. Use `(uint8_t[]){LOCAL_DEVICE_ID}, 1` to accept only native device.
`acceptedDeviceCount` make sure it matches device count in a `acceptedDeviceIDs` pointer. \
`waitTime_ms` specifies the wait time for new mesh packets when the queue is empty. Set it to `0` if this feature is not used.

---

### 3. Handle Incoming Packets

Override the weak callback (example):

```cpp
void meshPacket_handlePacketCallback(meshPacket_t *localPacket)
{
  switch(localPacket->packetType)
  {
    case PACKET_TYPE_CONTROL:
    {
      Serial.println("Control packet received!");
      break;
    }
    default:
    {
      Serial.printf("[ERROR]: S%02d, D%02d, invalid T%02d received\n", localPacket->sourceID, localPacket->destinationID, localPacket->packetType);
      break;
    }
  }
}
```

---

### 4. Sending a Message

```cpp
uint8_t payload[] = { 1, 2, 3, 4 };
meshPacket_sendMessage(LOCAL_DEVICE_ID, DEVICE_ID_MPPT_CONTROLLER, PACKET_TYPE_CONTROL, payload, sizeof(payload));
```

---

## License & Author

MIT / Beerware.

If you find this useful, buy the author a beer 🍺 :) \
**Dovydas Bružas, Lithuania** <Dovydisimo@gmail.com>

---

## TODO

To do's right now are inside .h file. Will move to README.md at some point.

