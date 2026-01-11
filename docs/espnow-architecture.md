# ESP-NOW System Architecture

## Complete System Overview

```mermaid
graph TB
    subgraph "ESP-NOW System Initialization"
        INIT[ESP-NOW Init] --> WIFI[WiFi STA+AP Mode]
        WIFI --> CHANNEL[Set Channel 11]
        CHANNEL --> CALLBACKS[Register Callbacks]
        CALLBACKS --> STATE[Allocate State Structure]
        STATE --> BROADCAST[Add Broadcast Peer FF:FF:FF:FF:FF:FF]
        BROADCAST --> TASK[Start Heartbeat Task]
    end

    subgraph "Core State Management"
        STATE --> |304KB| ESPNOW_STATE[EspNowState Structure]
        ESPNOW_STATE --> DEVICES[Device Registry 16 slots]
        ESPNOW_STATE --> UNPAIRED[Unpaired Devices 16 slots]
        ESPNOW_STATE --> CHUNKS[Chunk Buffers 4 slots]
        ESPNOW_STATE --> RETRY[Retry Queue 8 slots]
        ESPNOW_STATE --> HISTORY[Message History MESH_PEER_MAX]
        ESPNOW_STATE --> METRICS[Router Metrics]
    end

    subgraph "Message Flow - Transmit"
        TX_START[Application Send] --> ROUTER[routerSend]
        ROUTER --> SIZE_CHECK{Message Size?}
        SIZE_CHECK --> |< 200 bytes| V2_SMALL[sendV2Unfragmented]
        SIZE_CHECK --> |>= 200 bytes| V2_FRAG[sendV2Fragmented]
        
        V2_SMALL --> MODE_CHECK{Mode?}
        V2_FRAG --> MODE_CHECK
        
        MODE_CHECK --> |Direct| SINGLE[Send to 1 MAC]
        MODE_CHECK --> |Mesh| MULTI[Send to All Peers]
        
        SINGLE --> ENCRYPT_CHECK{Encrypted Peer?}
        MULTI --> PATH_FILTER[Filter by Path]
        PATH_FILTER --> ENCRYPT_CHECK
        
        ENCRYPT_CHECK --> |Yes| HW_ENCRYPT[ESP-NOW HW Encrypt]
        ENCRYPT_CHECK --> |No| HW_PLAIN[ESP-NOW Plain]
        
        HW_ENCRYPT --> ESP_NOW_SEND[esp_now_send]
        HW_PLAIN --> ESP_NOW_SEND
        ESP_NOW_SEND --> TX_CALLBACK[onEspNowDataSent]
    end

    subgraph "Message Flow - Receive"
        RX_START[ESP-NOW RX IRQ] --> RX_CALLBACK[onEspNowDataRecv]
        RX_CALLBACK --> RX_DECRYPT{Encrypted?}
        RX_DECRYPT --> |Yes| HW_DECRYPT[ESP-NOW HW Decrypt]
        RX_DECRYPT --> |No| RX_PLAIN[Plain Data]
        
        HW_DECRYPT --> PARSE[Parse JSON]
        RX_PLAIN --> PARSE
        
        PARSE --> MSG_TYPE{Message Type?}
        
        MSG_TYPE --> |HB| HEARTBEAT[Process Heartbeat]
        MSG_TYPE --> |ACK| ACK_HANDLER[Process ACK]
        MSG_TYPE --> |FRAG| FRAG_HANDLER[Reassemble Fragments]
        MSG_TYPE --> |CMD| CMD_HANDLER[Execute Command]
        MSG_TYPE --> |TEXT| TEXT_HANDLER[Store Message]
        MSG_TYPE --> |RESPONSE| RESP_HANDLER[Display Response]
        MSG_TYPE --> |TOPO_REQ| TOPO_REQ_HANDLER[Topology Request]
        MSG_TYPE --> |TOPO_RESP| TOPO_RESP_HANDLER[Topology Response]
        
        HEARTBEAT --> MESH_PEER[Update Mesh Peer Health]
        FRAG_HANDLER --> COMPLETE{Complete?}
        COMPLETE --> |Yes| REASSEMBLE[Reassemble & Process]
        COMPLETE --> |No| WAIT[Wait for More]
    end

    subgraph "Heartbeat System"
        TASK --> HB_LOOP[10s Interval Loop]
        HB_LOOP --> HB_MODE{Heartbeat Mode?}
        
        HB_MODE --> |Public| HB_BROADCAST[Broadcast to FF:FF:FF:FF:FF:FF]
        HB_MODE --> |Private| HB_CHECK{Has Peers?}
        
        HB_CHECK --> |Yes| HB_PEERS[Send to Each Paired Peer]
        HB_CHECK --> |No| HB_SKIP[Skip - No Peers]
        
        HB_BROADCAST --> HB_UNENCRYPTED[Unencrypted JSON]
        HB_PEERS --> HB_V2[v2 Transport with Encryption]
    end

    subgraph "Mesh Routing"
        MESH_RX[Receive Mesh Message] --> TTL_CHECK{TTL > 0?}
        TTL_CHECK --> |No| DROP[Drop Message]
        TTL_CHECK --> |Yes| DEDUP{Seen Before?}
        
        DEDUP --> |Yes| DROP
        DEDUP --> |No| DEST_CHECK{For Me?}
        
        DEST_CHECK --> |Yes| PROCESS[Process Locally]
        DEST_CHECK --> |No| FORWARD[Forward to Peers]
        
        FORWARD --> TTL_DEC[Decrement TTL]
        TTL_DEC --> PATH_ADD[Add Self to Path]
        PATH_ADD --> MESH_SEND[meshSendEnvelopeToPeers]
        MESH_SEND --> PATH_FILTER
    end

    subgraph "Device Pairing"
        PAIR_START[espnow pair/pairsecure] --> RESOLVE[Resolve MAC/Name]
        RESOLVE --> CHECK_EXIST{Already Paired?}
        CHECK_EXIST --> |Yes| REMOVE[Remove Old Peer]
        CHECK_EXIST --> |No| ADD_NEW[Add New Peer]
        REMOVE --> ADD_NEW
        
        ADD_NEW --> PAIR_TYPE{Secure?}
        PAIR_TYPE --> |Yes| DERIVE_KEY[Derive Encryption Key]
        PAIR_TYPE --> |No| NO_KEY[No Encryption]
        
        DERIVE_KEY --> ADD_PEER[esp_now_add_peer with encrypt=true]
        NO_KEY --> ADD_PEER_PLAIN[esp_now_add_peer with encrypt=false]
        
        ADD_PEER --> SAVE[Save to devices.json]
        ADD_PEER_PLAIN --> SAVE
    end

    subgraph "Fragmentation (v2)"
        LARGE_MSG[Message >= 200 bytes] --> CALC_FRAGS[Calculate Fragment Count]
        CALC_FRAGS --> FRAG_LOOP[For Each Fragment]
        FRAG_LOOP --> BUILD_FRAG[Build Fragment JSON]
        BUILD_FRAG --> |frag, total, msgId| SEND_FRAG[Send Fragment]
        SEND_FRAG --> FRAG_DONE{More?}
        FRAG_DONE --> |Yes| FRAG_LOOP
        FRAG_DONE --> |No| FRAG_COMPLETE[All Sent]
        
        RX_FRAG[Receive Fragment] --> FIND_BUF{Buffer Exists?}
        FIND_BUF --> |No| CREATE_BUF[Create Reassembly Buffer]
        FIND_BUF --> |Yes| USE_BUF[Use Existing Buffer]
        CREATE_BUF --> STORE_FRAG[Store Fragment]
        USE_BUF --> STORE_FRAG
        STORE_FRAG --> CHECK_COMPLETE{All Received?}
        CHECK_COMPLETE --> |Yes| REASSEMBLE_MSG[Reassemble Message]
        CHECK_COMPLETE --> |No| WAIT_MORE[Wait for More]
    end

    subgraph "Reliability (ACK/Dedup)"
        SEND_REL[Send with Reliability] --> REG_ACK[Register ACK Waiter]
        REG_ACK --> SEND_MSG[Send Message]
        SEND_MSG --> WAIT_ACK[Wait for ACK 800ms]
        WAIT_ACK --> GOT_ACK{ACK Received?}
        GOT_ACK --> |Yes| SUCCESS[Success]
        GOT_ACK --> |No| TIMEOUT[ACK Timeout]
        
        RX_REL[Receive Reliable Message] --> SEND_ACK[Send ACK Response]
        SEND_ACK --> DEDUP_CHECK{Seen msgId?}
        DEDUP_CHECK --> |Yes| DROP_DUP[Drop Duplicate]
        DEDUP_CHECK --> |No| PROCESS_NEW[Process Message]
    end

    subgraph "Topology Discovery"
        TOPO_START[espnow meshtopo] --> GEN_REQ_ID[Generate Request ID]
        GEN_REQ_ID --> BUILD_REQ[Build TOPO_REQ]
        BUILD_REQ --> SEND_REQ[Broadcast Request]
        
        RX_TOPO_REQ[Receive TOPO_REQ] --> ROLE_CHECK{Am I Master?}
        ROLE_CHECK --> |No| FWD_REQ[Forward Request]
        ROLE_CHECK --> |Yes| COLLECT[Collect Responses]
        
        FWD_REQ --> SEND_RESP[Send TOPO_RESP]
        SEND_RESP --> |START| RESP_START[Response Start]
        SEND_RESP --> |PEER| RESP_PEER[Peer Info]
        SEND_RESP --> |END| RESP_END[Response End]
        
        COLLECT --> TIMEOUT{30s Timeout?}
        TIMEOUT --> |Yes| DISPLAY[Display Topology]
        TIMEOUT --> |No| COLLECT
    end

    subgraph "Command Execution"
        RX_CMD[Receive CMD Message] --> AUTH{Authorized?}
        AUTH --> |No| REJECT[Reject]
        AUTH --> |Yes| QUEUE_CMD[Queue in cmd_exec Task]
        QUEUE_CMD --> EXEC[Execute Command]
        EXEC --> RESULT[Get Result]
        RESULT --> CHUNK_RESP{Large Response?}
        CHUNK_RESP --> |Yes| FRAG_RESP[Fragment Response]
        CHUNK_RESP --> |No| SEND_RESP[Send RESPONSE]
    end

    subgraph "Message History"
        STORE_MSG[Store Message] --> FIND_HIST{History Exists?}
        FIND_HIST --> |No| CREATE_HIST[Create History Buffer]
        FIND_HIST --> |Yes| USE_HIST[Use Existing]
        CREATE_HIST --> ADD_MSG[Add to Ring Buffer]
        USE_HIST --> ADD_MSG
        ADD_MSG --> INCR_SEQ[Increment Sequence]
        INCR_SEQ --> TRIM{Buffer Full?}
        TRIM --> |Yes| EVICT[Evict Oldest]
        TRIM --> |No| DONE[Done]
    end

    subgraph "Retry Queue"
        SEND_FAIL[Send Failed] --> RETRY_CHECK{Max Retries?}
        RETRY_CHECK --> |Exceeded| GIVE_UP[Give Up]
        RETRY_CHECK --> |Not Exceeded| ENQUEUE[Add to Retry Queue]
        ENQUEUE --> RETRY_LOOP[Retry Loop in Task]
        RETRY_LOOP --> RETRY_TIME{Time to Retry?}
        RETRY_TIME --> |Yes| RESEND[Resend Message]
        RETRY_TIME --> |No| WAIT_RETRY[Wait]
        RESEND --> RETRY_RESULT{Success?}
        RETRY_RESULT --> |Yes| DEQUEUE[Remove from Queue]
        RETRY_RESULT --> |No| INCR_RETRY[Increment Retry Count]
        INCR_RETRY --> RETRY_CHECK
    end

    style INIT fill:#4a9eff
    style ROUTER fill:#ff6b6b
    style RX_CALLBACK fill:#51cf66
    style HEARTBEAT fill:#ffd43b
    style MESH_SEND fill:#ff8787
    style PAIR_START fill:#cc5de8
    style TOPO_START fill:#20c997
```

## Message Type Flow Details

```mermaid
stateDiagram-v2
    [*] --> Initialized
    
    Initialized --> DirectMode: espnow mode direct
    Initialized --> MeshMode: espnow mode mesh
    
    DirectMode --> SendingDirect: Send Message
    SendingDirect --> EncryptCheck: Check Peer
    EncryptCheck --> EncryptedSend: Peer has encrypt=true
    EncryptCheck --> PlainSend: Peer has encrypt=false
    EncryptedSend --> [*]
    PlainSend --> [*]
    
    MeshMode --> SendingMesh: Send Message
    SendingMesh --> BroadcastPeers: Send to All Paired
    BroadcastPeers --> PathFilter: Filter by Path
    PathFilter --> PerPeerEncrypt: Per-Peer Encryption
    PerPeerEncrypt --> [*]
    
    MeshMode --> ReceivingMesh: Receive Message
    ReceivingMesh --> TTLCheck: Check TTL
    TTLCheck --> Drop: TTL=0
    TTLCheck --> DedupCheck: TTL>0
    DedupCheck --> Drop: Seen Before
    DedupCheck --> DestCheck: New Message
    DestCheck --> ProcessLocal: dst=me
    DestCheck --> ForwardMesh: dst=other
    ForwardMesh --> DecrementTTL
    DecrementTTL --> AddPath
    AddPath --> BroadcastPeers
    ProcessLocal --> [*]
    Drop --> [*]
```

## Heartbeat State Machine

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> CheckMode: 10s Timer
    
    CheckMode --> PublicMode: meshHeartbeatBroadcast=true
    CheckMode --> PrivateMode: meshHeartbeatBroadcast=false
    
    PublicMode --> BroadcastHB
    BroadcastHB --> Unencrypted
    Unencrypted --> UpdateSelf
    
    PrivateMode --> CheckPeers
    CheckPeers --> SkipHB: 0 Peers
    CheckPeers --> SendToPeers: Has Peers
    SendToPeers --> V2Transport
    V2Transport --> PerPeerEncrypt
    PerPeerEncrypt --> UpdateSelf
    SkipHB --> UpdateSelf
    
    UpdateSelf --> CleanupStale
    CleanupStale --> Idle
    
    note right of BroadcastHB: Send to FF:FF:FF:FF:FF:FF
    note right of Unencrypted: Plain JSON
    note right of CheckPeers: Count Paired Peers
    note right of V2Transport: Use v2 Protocol
    note right of PerPeerEncrypt: Encrypt per Peer
```

## Fragmentation Reassembly

```mermaid
sequenceDiagram
    participant Sender
    participant Router
    participant Transport
    participant Receiver
    participant Reassembly
    
    Sender->>Router: Send 500 byte message
    Router->>Router: Calculate fragments (3)
    Router->>Transport: Fragment 1/3 (frag:0, total:3)
    Transport->>Receiver: esp_now_send
    Router->>Transport: Fragment 2/3 (frag:1, total:3)
    Transport->>Receiver: esp_now_send
    Router->>Transport: Fragment 3/3 (frag:2, total:3)
    Transport->>Receiver: esp_now_send
    
    Receiver->>Reassembly: Store Fragment 1
    Receiver->>Reassembly: Store Fragment 2
    Receiver->>Reassembly: Store Fragment 3
    Reassembly->>Reassembly: Check Complete (3/3)
    Reassembly->>Reassembly: Reassemble Message
    Reassembly->>Receiver: Process Complete Message
```

## Device Pairing Flow

```mermaid
sequenceDiagram
    participant User
    participant CLI
    participant ESP-NOW
    participant Peer
    
    User->>CLI: espnow setpassphrase mypass123
    CLI->>ESP-NOW: Derive Key from Passphrase
    ESP-NOW->>ESP-NOW: Store derivedKey[16]
    
    User->>CLI: espnow pairsecure AA:BB:CC:DD:EE:FF
    CLI->>ESP-NOW: Add Encrypted Peer
    ESP-NOW->>ESP-NOW: esp_now_add_peer(encrypt=true, lmk=derivedKey)
    ESP-NOW->>ESP-NOW: Save to devices.json
    
    ESP-NOW->>Peer: Send Message
    Note over ESP-NOW,Peer: ESP-NOW HW encrypts with LMK
    Peer->>ESP-NOW: Send Response
    Note over ESP-NOW,Peer: ESP-NOW HW decrypts with LMK
```

## Topology Discovery Flow

```mermaid
sequenceDiagram
    participant Master
    participant Worker1
    participant Worker2
    participant Worker3
    
    Master->>Master: espnow meshtopo
    Master->>Master: Generate reqId=12345
    Master->>Worker1: TOPO_REQ (reqId=12345, ttl=3)
    Master->>Worker2: TOPO_REQ (reqId=12345, ttl=3)
    
    Worker1->>Worker3: TOPO_REQ (reqId=12345, ttl=2)
    Worker2->>Worker3: TOPO_REQ (reqId=12345, ttl=2)
    
    Worker1->>Master: TOPO_RESP START (reqId=12345, totalPeers=2)
    Worker1->>Master: TOPO_RESP PEER (Worker3 info)
    Worker1->>Master: TOPO_RESP PEER (Master info)
    Worker1->>Master: TOPO_RESP END
    
    Worker2->>Master: TOPO_RESP START (reqId=12345, totalPeers=2)
    Worker2->>Master: TOPO_RESP PEER (Worker3 info)
    Worker2->>Master: TOPO_RESP PEER (Master info)
    Worker2->>Master: TOPO_RESP END
    
    Worker3->>Master: TOPO_RESP START (reqId=12345, totalPeers=2)
    Worker3->>Master: TOPO_RESP PEER (Worker1 info)
    Worker3->>Master: TOPO_RESP PEER (Worker2 info)
    Worker3->>Master: TOPO_RESP END
    
    Master->>Master: Wait 30s for all responses
    Master->>Master: Build topology graph
    Master->>Master: Display results
```

## Key Data Structures

### EspNowState (304KB)
- **initialized**: bool
- **channel**: uint8_t (11)
- **mode**: ESPNOW_MODE_DIRECT | ESPNOW_MODE_MESH
- **devices[16]**: Device registry with MAC, name, encrypted flag, key[16]
- **unpairedDevices[16]**: Discovered but not paired
- **chunkBuffers[4]**: Fragment reassembly (max 4 concurrent)
- **retryQueue[8]**: Failed message retry
- **peerMessageHistories[16]**: Per-device message history (30-100 messages each)
- **routerMetrics**: Counters for sent/received/failed/retried
- **derivedKey[16]**: Encryption key from passphrase

### Message Types
- **HB**: Heartbeat (mesh peer discovery)
- **ACK**: Acknowledgment (reliability)
- **FRAG**: Fragment (large message piece)
- **CMD**: Remote command execution
- **TEXT**: Text message
- **RESPONSE**: Command response
- **TOPO_REQ**: Topology discovery request
- **TOPO_RESP**: Topology discovery response
- **MESH_SYS**: Mesh system message

### Transport Modes
- **v2 Small**: < 200 bytes, single packet, optional ACK
- **v2 Fragmented**: >= 200 bytes, multiple fragments, reassembly required

### Encryption
- **Hardware-based**: ESP-NOW uses AES-128 with LMK (Local Master Key)
- **Per-peer**: Each peer has encrypt flag and optional 16-byte key
- **Automatic**: Encryption/decryption handled by ESP-NOW hardware layer
