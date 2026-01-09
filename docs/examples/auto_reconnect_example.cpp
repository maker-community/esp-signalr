/**
 * ESP32 SignalR Auto-Reconnect Example
 * 
 * This example demonstrates how to use the auto-reconnect feature
 * with skip negotiation for WebSocket-only connections.
 */

#include "hub_connection_builder.h"
#include "signalr_client_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "SIGNALR_EXAMPLE";

// Global connection object
static signalr::hub_connection* g_connection = nullptr;

/**
 * Configure and create a SignalR connection with auto-reconnect
 */
void setup_signalr_connection()
{
    ESP_LOGI(TAG, "Setting up SignalR connection with auto-reconnect...");
    
    // Create connection with automatic reconnect enabled
    // This uses the default reconnect delays: 0s, 2s, 10s, 30s (matching C# and JS clients)
    auto connection = signalr::hub_connection_builder()
        .with_url("wss://your-signalr-server.com/signalrhub")
        .skip_negotiation()  // Skip negotiation - directly use WebSocket
        .with_automatic_reconnect()  // Enable auto-reconnect with default delays
        .build();
    
    // Alternatively, use custom reconnect delays:
    // std::vector<std::chrono::milliseconds> custom_delays = {
    //     std::chrono::seconds(0),
    //     std::chrono::seconds(2),
    //     std::chrono::seconds(10),
    //     std::chrono::seconds(30)
    // };
    // auto connection = signalr::hub_connection_builder()
    //     .with_url("wss://your-server.com/hub")
    //     .skip_negotiation()
    //     .with_automatic_reconnect(custom_delays)
    //     .build();
    
    // Set up disconnected callback
    connection.set_disconnected([](std::exception_ptr ex) {
        try {
            if (ex) {
                std::rethrow_exception(ex);
            }
            ESP_LOGW(TAG, "Connection closed gracefully");
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Connection lost: %s", e.what());
        }
        
        ESP_LOGI(TAG, "Auto-reconnect will attempt to restore the connection...");
    });
    
    // Register server method handlers
    connection.on("ReceiveMessage", [](const std::vector<signalr::value>& args) {
        if (args.size() >= 2 && args[0].is_string() && args[1].is_string()) {
            std::string user = args[0].as_string();
            std::string message = args[1].as_string();
            ESP_LOGI(TAG, "Message from %s: %s", user.c_str(), message.c_str());
        }
    });
    
    connection.on("UpdateStatus", [](const std::vector<signalr::value>& args) {
        if (!args.empty() && args[0].is_string()) {
            ESP_LOGI(TAG, "Status update: %s", args[0].as_string().c_str());
        }
    });
    
    connection.on("DeviceCommand", [](const std::vector<signalr::value>& args) {
        if (!args.empty() && args[0].is_string()) {
            std::string command = args[0].as_string();
            ESP_LOGI(TAG, "Received device command: %s", command.c_str());
            
            // Handle the command
            if (command == "reboot") {
                ESP_LOGW(TAG, "Reboot command received!");
                // Implement reboot logic
            } else if (command == "status") {
                ESP_LOGI(TAG, "Status command received, sending status...");
                // Send status back to server
            }
        }
    });
    
    // Store connection for later use
    g_connection = &connection;
    
    // Start the connection
    ESP_LOGI(TAG, "Starting SignalR connection...");
    connection.start([](std::exception_ptr ex) {
        if (ex) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Failed to start connection: %s", e.what());
            }
        } else {
            ESP_LOGI(TAG, "SignalR connection started successfully!");
            ESP_LOGI(TAG, "Connection ID: %s", g_connection->get_connection_id().c_str());
            
            // Send initial message to server
            send_device_online_notification();
        }
    });
}

/**
 * Send a message to the server
 */
void send_message_to_server(const std::string& method_name, const std::string& message)
{
    if (g_connection == nullptr) {
        ESP_LOGE(TAG, "Connection not initialized");
        return;
    }
    
    if (g_connection->get_connection_state() != signalr::connection_state::connected) {
        ESP_LOGW(TAG, "Connection not in connected state, message will be dropped");
        return;
    }
    
    std::vector<signalr::value> args { message };
    
    g_connection->send(method_name, args, [method_name](std::exception_ptr ex) {
        if (ex) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Failed to send %s: %s", method_name.c_str(), e.what());
            }
        } else {
            ESP_LOGI(TAG, "Successfully sent %s", method_name.c_str());
        }
    });
}

/**
 * Invoke a server method and wait for response
 */
void invoke_server_method(const std::string& method_name, int param)
{
    if (g_connection == nullptr) {
        ESP_LOGE(TAG, "Connection not initialized");
        return;
    }
    
    if (g_connection->get_connection_state() != signalr::connection_state::connected) {
        ESP_LOGW(TAG, "Connection not in connected state");
        return;
    }
    
    std::vector<signalr::value> args { param };
    
    g_connection->invoke(method_name, args, 
        [method_name](const signalr::value& result, std::exception_ptr ex) {
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Failed to invoke %s: %s", method_name.c_str(), e.what());
                }
            } else {
                if (result.is_double()) {
                    ESP_LOGI(TAG, "%s returned: %f", method_name.c_str(), result.as_double());
                } else if (result.is_string()) {
                    ESP_LOGI(TAG, "%s returned: %s", method_name.c_str(), result.as_string().c_str());
                } else {
                    ESP_LOGI(TAG, "%s completed successfully", method_name.c_str());
                }
            }
        });
}

/**
 * Send device online notification
 */
void send_device_online_notification()
{
    ESP_LOGI(TAG, "Sending device online notification...");
    send_message_to_server("DeviceOnline", "ESP32 device is now online");
}

/**
 * Stop the connection gracefully
 */
void stop_signalr_connection()
{
    if (g_connection == nullptr) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping SignalR connection...");
    
    g_connection->stop([](std::exception_ptr ex) {
        if (ex) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Error while stopping: %s", e.what());
            }
        } else {
            ESP_LOGI(TAG, "Connection stopped successfully");
        }
    });
}

/**
 * Main application task
 */
void app_main(void)
{
    // Initialize WiFi first (not shown here)
    // wifi_init_sta();
    
    // Wait for WiFi connection
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    // wait_for_wifi();
    
    // Setup SignalR connection with auto-reconnect
    setup_signalr_connection();
    
    // Main loop - send periodic updates
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // Wait 30 seconds
        
        if (g_connection && 
            g_connection->get_connection_state() == signalr::connection_state::connected) {
            
            // Send periodic heartbeat or status update
            send_message_to_server("Heartbeat", "Device is alive");
            
            // Or invoke a method with parameters
            int sensor_value = 42; // Read from actual sensor
            invoke_server_method("UpdateSensorData", sensor_value);
        } else {
            ESP_LOGW(TAG, "Connection not ready, skipping update (auto-reconnect is active)");
        }
    }
}

/**
 * Simulation: Test reconnection by temporarily disabling WiFi
 */
void test_reconnection()
{
    ESP_LOGI(TAG, "Testing reconnection...");
    
    // Simulate network loss
    ESP_LOGW(TAG, "Simulating network loss...");
    // wifi_disconnect();
    
    vTaskDelay(pdMS_TO_TICKS(10000)); // Wait 10 seconds
    
    // Restore network
    ESP_LOGI(TAG, "Restoring network...");
    // wifi_reconnect();
    
    // Auto-reconnect should kick in automatically
    ESP_LOGI(TAG, "Auto-reconnect should now attempt to restore the connection");
}
