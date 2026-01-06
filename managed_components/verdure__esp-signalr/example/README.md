# SignalR Example for ESP32

This example demonstrates how to use the ESP32 SignalR client library to connect to an ASP.NET Core SignalR server.

## Configuration

Before building, update the following in `main/signalr_example.cpp`:

```cpp
#define WIFI_SSID      "YOUR_SSID"           // Your WiFi SSID
#define WIFI_PASSWORD  "YOUR_PASSWORD"        // Your WiFi password
#define SIGNALR_HUB_URL "http://your-server.com:5000/chatHub"  // Your SignalR server URL
```

## Requirements

- ESP-IDF >= 5.0
- ASP.NET Core SignalR test server (see ../TEST_SERVER.md)

## Build and Flash

```bash
# Configure for your ESP32 board
idf.py set-target esp32

# Build the project
idf.py build

# Flash to your ESP32
idf.py flash monitor
```

## Expected Output

```
I (xxx) SIGNALR_EXAMPLE: ESP32 SignalR Client Example
I (xxx) SIGNALR_EXAMPLE: Connected to WiFi SSID:YourSSID
I (xxx) SIGNALR_EXAMPLE: Creating SignalR connection...
I (xxx) SIGNALR_EXAMPLE: Connected to SignalR hub successfully!
I (xxx) SIGNALR_EXAMPLE: Message sent successfully
I (xxx) SIGNALR_EXAMPLE: Message from ESP32: Hello from ESP32 SignalR client!
```

## Troubleshooting

### WiFi Connection Fails
- Verify SSID and password are correct
- Check WiFi signal strength
- Ensure ESP32 is within range

### SignalR Connection Fails
- Verify server URL is accessible from ESP32
- Check server is running and accepting connections
- Ensure CORS is configured on server
- Try HTTP first before HTTPS

### Memory Issues
- Monitor free heap with `esp_get_free_heap_size()`
- Enable PSRAM if available: `CONFIG_SPIRAM_SUPPORT=y`
- Reduce buffer sizes if needed

## Server Setup

See `../TEST_SERVER.md` for instructions on setting up an ASP.NET Core SignalR test server.

## Features Demonstrated

1. WiFi connection and event handling
2. SignalR hub connection creation
3. Registering message handlers
4. Starting the connection
5. Invoking hub methods
6. Sending messages to the server
7. Receiving messages from the server

## Next Steps

- Implement error handling and reconnection logic
- Add authentication if needed
- Customize message handling for your application
- Implement bi-directional communication patterns

## Support

For issues and questions:
- GitHub Issues: https://github.com/maker-community/esp-signalr/issues
- Documentation: https://github.com/maker-community/esp-signalr
