/*
 * ModbusTCP Master to KNX BAOS Gateway
 * ESP32-S3 as ModbusTCP MASTER (Client) + KNX BAOS Module 832
 * 
 * ESP32 acts as Modbus TCP Master connecting to multiple Modbus slaves
 * Reads data from slaves and writes to KNX datapoints ONLY when values change
 * Reads KNX datapoints and writes to Modbus slaves
 */

#include <WiFi.h>
#include <ModbusIP_ESP8266.h>
#include "knx_BAOS.h"

// WiFi Configuration
const char* ssid = "YOURWIFINAME";
const char* password = "YOURPASSWORD";

// Modbus TCP Master
ModbusIP mb;

// KNX Configuration
#define MAX_DATAPOINTS 1000

// Datapoint type mapping
uint8_t dpTypeMap[MAX_DATAPOINTS];
uint16_t lastDpValues[MAX_DATAPOINTS]; 
bool dpHasValue[MAX_DATAPOINTS];

// Anti-loop tracking: track last source of change
enum ChangeSource {
  SOURCE_NONE = 0,
  SOURCE_MODBUS = 1,
  SOURCE_KNX = 2
};

struct ValueTracking {
  uint16_t value;
  ChangeSource lastSource;
  unsigned long lastChangeTime;
} valueTracking[MAX_DATAPOINTS];

#define ANTI_LOOP_TIMEOUT 500  // Don't echo back within 500ms

// Modbus Slave Configuration
#define MAX_SLAVES 10

struct ModbusSlave {
  IPAddress ip;              // Slave IP address
  uint8_t slaveId;          // Modbus slave ID
  bool enabled;             // Is this slave active?
  unsigned long lastPoll;   // Last poll timestamp
  uint16_t pollInterval;    // Polling interval in ms
  bool connected;           // Connection status
  
  // Mapping configuration
  struct RegisterMap {
    uint16_t modbusStartAddr;  // Starting address on slave
    uint16_t modbusCount;      // Number of registers/coils
    uint16_t knxStartDp;       // Starting KNX datapoint
    uint8_t registerType;      // 0=Coil, 3=HoldingReg
    bool readFromSlave;        // true=read from slave to KNX
    bool writeToSlave;         // true=write from KNX to slave
  } maps[5];  // Each slave can have up to 5 mappings
  
  uint8_t mapCount;
};

ModbusSlave slaves[MAX_SLAVES];
uint8_t slaveCount = 0;

// Gateway state
bool knxInitialized = false;
unsigned long lastKnxCheck = 0;
const unsigned long KNX_CHECK_INTERVAL = 30000;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ModbusTCP Master-KNX Gateway ===");
  delay(500);
  
  // Initialize value cache
  for (int i = 0; i < MAX_DATAPOINTS; i++) {
    lastDpValues[i] = 0;
    dpHasValue[i] = false;
    valueTracking[i].value = 0;
    valueTracking[i].lastSource = SOURCE_NONE;
    valueTracking[i].lastChangeTime = 0;
  }
  
  // Initialize KNX BAOS
  Serial.println("Initializing KNX BAOS...");
  knxBaos_initHardware();
  delay(1000);
  knxBaos_initLayer_Baos();
  delay(2000);
  knxInitialized = true;
  Serial.println("KNX BAOS initialized");
  
  // Connect to WiFi
  Serial.print("Connecting to WiFi");
  IPAddress ip(192, 168, 1, 104);
  WiFi.config(ip);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Initialize Modbus Master (client mode)
  mb.client();
  
  // Configure Modbus slaves
  configureSlaves();
  
  Serial.println("ModbusTCP Master initialized");
  Serial.print("Configured ");
  Serial.print(slaveCount);
  Serial.println(" slaves");
  
  Serial.println("Gateway ready!");
}

void loop() {
  // Process Modbus transactions
  mb.task();
  
  // Poll all configured slaves
  pollModbusSlaves();
  
  // Process KNX responses (KNX -> Modbus direction)
  processKnxResponses();
  
  // Periodic KNX health check
  if (millis() - lastKnxCheck > KNX_CHECK_INTERVAL) {
    lastKnxCheck = millis();
    checkHealth();
  }
}

// Configure Modbus slaves and their mappings
void configureSlaves() {
  slaveCount = 0;
  
  // ==================== SLAVE 1 ====================
  slaves[0].ip = IPAddress(192, 168, 1, 100);
  slaves[0].slaveId = 1;
  slaves[0].enabled = true;
  slaves[0].pollInterval = 1000;  // Poll every 1 second
  slaves[0].connected = false;
  slaves[0].mapCount = 1;
  
  // 
  // Modbus -> KNX: Read registers and send to KNX when changed
  // KNX -> Modbus: Read KNX indications and write to registers
  slaves[0].maps[0].modbusStartAddr = 0;
  slaves[0].maps[0].modbusCount = 20;
  slaves[0].maps[0].knxStartDp = 0;
  slaves[0].maps[0].registerType = 3;  // Holding Register
  slaves[0].maps[0].readFromSlave = true;   // Read from Modbus -> write to KNX
  slaves[0].maps[0].writeToSlave = true;    // Read from KNX -> write to Modbus
  
  slaveCount++;
  
  Serial.println("\n=== Slave Configuration ===");
  for (uint8_t i = 0; i < slaveCount; i++) {
    Serial.print("Slave ");
    Serial.print(i + 1);
    Serial.print(": ID=");
    Serial.print(slaves[i].slaveId);
    Serial.print(" IP=");
    Serial.print(slaves[i].ip);
    Serial.print(" Poll=");
    Serial.print(slaves[i].pollInterval);
    Serial.println("ms");
    
    for (uint8_t m = 0; m < slaves[i].mapCount; m++) {
      Serial.print("  Map ");
      Serial.print(m + 1);
      Serial.print(": ");
      
      const char* regTypes[] = {"Coil", "N/A", "N/A", "HoldingReg"};
      Serial.print(regTypes[slaves[i].maps[m].registerType]);
      Serial.print(" Modbus[");
      Serial.print(slaves[i].maps[m].modbusStartAddr);
      Serial.print("-");
      Serial.print(slaves[i].maps[m].modbusStartAddr + slaves[i].maps[m].modbusCount - 1);
      Serial.print("] <-> KNX DP[");
      Serial.print(slaves[i].maps[m].knxStartDp);
      Serial.print("-");
      Serial.print(slaves[i].maps[m].knxStartDp + slaves[i].maps[m].modbusCount - 1);
      Serial.print("] ");
      
      if (slaves[i].maps[m].readFromSlave && slaves[i].maps[m].writeToSlave) {
        Serial.print("BIDIRECTIONAL");
      } else if (slaves[i].maps[m].readFromSlave) {
        Serial.print("Modbus->KNX");
      } else if (slaves[i].maps[m].writeToSlave) {
        Serial.print("KNX->Modbus");
      }
      Serial.println();
    }
  }
  Serial.println("===========================\n");
}

// Poll all configured Modbus slaves
void pollModbusSlaves() {
  unsigned long now = millis();
  
  for (uint8_t i = 0; i < slaveCount; i++) {
    if (!slaves[i].enabled) continue;
    
    // Check if it's time to poll this slave
    if (now - slaves[i].lastPoll >= slaves[i].pollInterval) {
      slaves[i].lastPoll = now;
      
      // Poll each mapping for this slave
      for (uint8_t m = 0; m < slaves[i].mapCount; m++) {
        if (slaves[i].maps[m].readFromSlave) {
          readFromModbusSlave(i, m);
        }
      }
    }
  }
}

// Storage for read values
static bool coilValues[100];
static uint16_t regValues[100];

// Read data from a Modbus slave
void readFromModbusSlave(uint8_t slaveIndex, uint8_t mapIndex) {
  ModbusSlave &slave = slaves[slaveIndex];
  ModbusSlave::RegisterMap &map = slave.maps[mapIndex];
  
  // Connect if needed
  if (!mb.isConnected(slave.ip)) {
    if (!mb.connect(slave.ip)) {
      slave.connected = false;
      Serial.print("Failed to connect to slave ");
      Serial.println(slaveIndex + 1);
      return;
    }
    Serial.print("Connected to slave ");
    Serial.println(slaveIndex + 1);
  }
  slave.connected = true;
  
  // Read based on register type
  switch (map.registerType) {
    case 0:  // Coil (FC01)
      {
        uint16_t result = mb.readCoil(slave.ip, map.modbusStartAddr, coilValues, 
                                       map.modbusCount, nullptr, slave.slaveId);
        if (result > 0) {
          for (uint16_t i = 0; i < map.modbusCount; i++) {
            handleModbusCoilRead(slaveIndex, mapIndex, i, coilValues[i] ? 1 : 0);
          }
        }
      }
      break;
      
    case 3:  // Holding Register (FC03)
      {
        uint16_t result = mb.readHreg(slave.ip, map.modbusStartAddr, regValues, 
                                       map.modbusCount, nullptr, slave.slaveId);
        if (result > 0) {
          for (uint16_t i = 0; i < map.modbusCount; i++) {
            handleModbusHoldingRegRead(slaveIndex, mapIndex, i, regValues[i]);
          }
        }
      }
      break;
  }
}

// Callback handlers for reading from Modbus slaves
uint16_t handleModbusCoilRead(uint8_t slaveIndex, uint8_t mapIndex, uint16_t offset, uint16_t val) {
  ModbusSlave::RegisterMap &map = slaves[slaveIndex].maps[mapIndex];
  uint16_t knxDp = map.knxStartDp + offset;
  uint16_t binaryVal = val ? 1 : 0;
  
  if (!dpHasValue[knxDp] || lastDpValues[knxDp] != binaryVal) {
    Serial.print("CHANGE: Slave ");
    Serial.print(slaveIndex + 1);
    Serial.print(" Coil[");
    Serial.print(map.modbusStartAddr + offset);
    Serial.print("] = ");
    Serial.print(binaryVal);
    Serial.print(" -> KNX DP ");
    Serial.println(knxDp);
    
    knxBaos_setDataPointBinary(knxDp, binaryVal);
    lastDpValues[knxDp] = binaryVal;
    dpHasValue[knxDp] = true;
  }
  return val;
}

uint16_t handleModbusHoldingRegRead(uint8_t slaveIndex, uint8_t mapIndex, uint16_t offset, uint16_t val) {
  ModbusSlave::RegisterMap &map = slaves[slaveIndex].maps[mapIndex];
  uint16_t knxDp = map.knxStartDp + offset;
  
  if (!dpHasValue[knxDp] || lastDpValues[knxDp] != val) {
    Serial.print("CHANGE: Slave ");
    Serial.print(slaveIndex + 1);
    Serial.print(" HoldingReg[");
    Serial.print(map.modbusStartAddr + offset);
    Serial.print("] = ");
    Serial.print(val);
    Serial.print(" -> KNX DP ");
    Serial.println(knxDp);
    
    knxBaos_setDataPoint2Byte(knxDp, val);
    
    lastDpValues[knxDp] = val;
    dpHasValue[knxDp] = true;
  }
  return val;
}

// Process incoming KNX BAOS responses (KNX -> Modbus direction)
void processKnxResponses() {
  static uint8_t rxBuffer[256];
  static uint8_t rxIndex = 0;
  
  while (serialKNX.available()) {
    uint8_t byte = serialKNX.read();
    
    if (byte == 0x68 && rxIndex == 0) {
      rxBuffer[rxIndex++] = byte;
    } else if (rxIndex > 0) {
      rxBuffer[rxIndex++] = byte;
      
      if (byte == 0x16 && rxIndex > 6) {
        // Debug: Print raw frame
        Serial.print("KNX RX [");
        Serial.print(rxIndex);
        Serial.print("]: ");
        for (uint8_t i = 0; i < rxIndex; i++) {
          if (rxBuffer[i] < 0x10) Serial.print("0");
          Serial.print(rxBuffer[i], HEX);
          Serial.print(" ");
        }
        Serial.println();
        
        processKnxFrame(rxBuffer, rxIndex);
        rxIndex = 0;
      }
      
      if (rxIndex >= sizeof(rxBuffer)) {
        Serial.println("Buffer overflow!");
        rxIndex = 0;
      }
    }
  }
}

void processKnxFrame(uint8_t* frame, uint8_t length) {
  if (length < 8) return;
  
  // Frame: 0x68 L L 0x68 CR MAIN_SERVICE SUB_SERVICE ...
  uint8_t mainService = frame[5];
  uint8_t subService = frame[6];
  
  // DatapointValue.Ind = 0xF0 (Main) + 0xC1 (Sub)
  if (mainService == 0xF0 && subService == 0xC1) {
    parseDatapointIndicationToModbus(frame, length);
  }
}

// Parse KNX datapoint indication according to BAOS protocol
// Frame structure: 0x68 L L 0x68 CR 0xF0 0xC1 START_DP_H START_DP_L NUM_DP_H NUM_DP_L DP_DATA... C 0x16
void parseDatapointIndicationToModbus(uint8_t* frame, uint8_t length) {
  if (length < 14) return;  // Minimum frame size for indication with 1 datapoint
  
  // According to BAOS protocol:
  // +0: 0x68
  // +1: Length
  // +2: Length
  // +3: 0x68
  // +4: Control byte (CR: 0xF3 odd, 0xD3 even from BAOS)
  // +5: Main Service (0xF0)
  // +6: Sub Service (0xC1)
  // +7: Start Datapoint ID High
  // +8: Start Datapoint ID Low
  // +9: Number of datapoints High
  // +10: Number of datapoints Low
  // +11...: Datapoint data blocks
  
  uint16_t startDatapointId = (frame[7] << 8) | frame[8];
  uint16_t numberOfDatapoints = (frame[9] << 8) | frame[10];
  
  Serial.print("DatapointValue.Ind: Start DP=");
  Serial.print(startDatapointId);
  Serial.print(", Count=");
  Serial.println(numberOfDatapoints);
  
  // Parse each datapoint in the indication
  uint8_t pos = 11;  // Start of datapoint data
  
  for (uint16_t dp = 0; dp < numberOfDatapoints && pos < length - 2; dp++) {
    // Each datapoint block: DP_ID_H DP_ID_L STATE LENGTH VALUE...
    if (pos + 4 > length - 2) break;
    
    uint16_t dataPointId = (frame[pos] << 8) | frame[pos + 1];
    uint8_t state = frame[pos + 2];
    uint8_t valueLength = frame[pos + 3];
    
    Serial.print("  DP ");
    Serial.print(dataPointId);
    Serial.print(": State=0x");
    Serial.print(state, HEX);
    Serial.print(", Len=");
    Serial.print(valueLength);
    
    if (pos + 4 + valueLength > length - 2) break;
    
    // Extract value based on length
    if (valueLength == 1) {
      uint8_t value = frame[pos + 4];
      Serial.print(", Value=");
      Serial.println(value);
      
      // Write to Modbus slave
      writeKnxValueToModbus(dataPointId, value, valueLength);
      
    } else if (valueLength == 2) {
      uint16_t value = (frame[pos + 4] << 8) | frame[pos + 5];
      Serial.print(", Value=");
      Serial.println(value);
      
      // Write to Modbus slave
      writeKnxValueToModbus(dataPointId, value, valueLength);
      
    } else {
      Serial.print(", Value=");
      for (uint8_t i = 0; i < valueLength; i++) {
        if (frame[pos + 4 + i] < 0x10) Serial.print("0");
        Serial.print(frame[pos + 4 + i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }
    
    // Move to next datapoint block
    pos += 4 + valueLength;
  }
}

// Write KNX value to appropriate Modbus slave
void writeKnxValueToModbus(uint16_t dataPointId, uint16_t value, uint8_t valueLength) {
  unsigned long now = millis();
  
  // Check if this change came from Modbus recently (anti-loop)
  if (valueTracking[dataPointId].lastSource == SOURCE_MODBUS && 
      (now - valueTracking[dataPointId].lastChangeTime) < ANTI_LOOP_TIMEOUT &&
      valueTracking[dataPointId].value == value) {
    // This is likely an echo from our own Modbus->KNX write, ignore
    Serial.print("  (Ignoring echo from Modbus for DP");
    Serial.print(dataPointId);
    Serial.println(")");
    return;
  }
  
  // Find which slave/map this datapoint belongs to
  for (uint8_t s = 0; s < slaveCount; s++) {
    if (!slaves[s].enabled || !slaves[s].connected) continue;
    
    for (uint8_t m = 0; m < slaves[s].mapCount; m++) {
      ModbusSlave::RegisterMap &map = slaves[s].maps[m];
      
      if (!map.writeToSlave) continue;
      
      // Check if this datapoint is in this map's range
      if (dataPointId >= map.knxStartDp && 
          dataPointId < map.knxStartDp + map.modbusCount) {
        
        uint16_t offset = dataPointId - map.knxStartDp;
        uint16_t modbusAddr = map.modbusStartAddr + offset;
        
        if (valueLength == 1) {
          // Binary value
          Serial.print("  -> Writing to Slave ");
          Serial.print(s + 1);
          Serial.print(" Coil[");
          Serial.print(modbusAddr);
          Serial.print("] = ");
          Serial.println(value);
          
          if (map.registerTypevalue == 0) {  // Coil
            mb.writeCoil(slaves[s].ip, modbusAddr, value != 0, nullptr, slaves[s].slaveId);
          }
          
        } else if (valueLength == 2) {
          // 2-byte value
          Serial.print("  -> Writing to Slave ");
          Serial.print(s + 1);
          Serial.print(" HoldingReg[");
          Serial.print(modbusAddr);
          Serial.print("] = ");
          Serial.println(value);
          
          if (map.registerType == 3) {  // Holding Register
            mb.writeHreg(slaves[s].ip, modbusAddr, value, nullptr, slaves[s].slaveId);
          }
        }
        
        // Update tracking
        valueTracking[dataPointId].value = value;
        valueTracking[dataPointId].lastSource = SOURCE_KNX;
        valueTracking[dataPointId].lastChangeTime = now;
        
        return;  // Found and processed
      }
    }
  }
  
  Serial.println("  -> No matching Modbus mapping found");
}

void checkHealth() {
  Serial.println("=== Health Check ===");
  
  // Print slave connection status
  for (uint8_t i = 0; i < slaveCount; i++) {
    Serial.print("Slave ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(slaves[i].connected ? "CONNECTED" : "DISCONNECTED");
  }
  
  Serial.println("====================");
}