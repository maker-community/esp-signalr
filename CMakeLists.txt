idf_component_register(
    SRCS 
        # ESP32 platform adapters
        "src/adapters/esp32_websocket_client.cpp"
        "src/adapters/esp32_http_client.cpp"
        "src/json_adapter.cpp"
        
        # SignalR core protocol
        "src/callback_manager.cpp"
        "src/cancellation_token.cpp"
        "src/cancellation_token_source.cpp"
        "src/connection_impl.cpp"
        "src/handshake_protocol.cpp"
        "src/hub_connection.cpp"
        "src/hub_connection_builder.cpp"
        "src/hub_connection_impl.cpp"
        "src/json_helpers.cpp"
        "src/json_hub_protocol.cpp"
        "src/logger.cpp"
        "src/negotiate.cpp"
        "src/signalr_client_config.cpp"
        "src/signalr_value.cpp"
        "src/trace_log_writer.cpp"
        "src/transport.cpp"
        "src/transport_factory.cpp"
        "src/url_builder.cpp"
        "src/websocket_transport.cpp"
        "src/signalr_default_scheduler.cpp"
        
        # URI utilities
        "third_party_code/cpprestsdk/uri.cpp"
        "third_party_code/cpprestsdk/uri_builder.cpp"
        
    INCLUDE_DIRS 
        "include"
        
    PRIV_INCLUDE_DIRS
        "third_party_code"
        "third_party_code/cpprestsdk"
        "src"
        
    REQUIRES 
        esp_http_client
        json
        freertos
        espressif__esp_websocket_client
)

# Add compile definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC 
    VERDURE_ESP_SIGNALR
    NO_SIGNALRCLIENT_EXPORTS
)

# Configure C++ standard and options
target_compile_options(${COMPONENT_LIB} PRIVATE 
    -std=c++11
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-unknown-pragmas
)
