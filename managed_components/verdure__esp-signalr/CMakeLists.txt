idf_component_register(
    SRCS 
        # ESP32 platform adapters
        "src/adapters/esp32_websocket_client.cpp"
        "src/adapters/esp32_http_client.cpp"
        "src/json_adapter.cpp"
        
        # SignalR core protocol (to be uncommented after extraction)
        # "src/signalrclient/callback_manager.cpp"
        # "src/signalrclient/cancellation_token.cpp"
        # "src/signalrclient/cancellation_token_source.cpp"
        # "src/signalrclient/connection_impl.cpp"
        # "src/signalrclient/handshake_protocol.cpp"
        # "src/signalrclient/hub_connection.cpp"
        # "src/signalrclient/hub_connection_builder.cpp"
        # "src/signalrclient/hub_connection_impl.cpp"
        # "src/signalrclient/json_helpers.cpp"
        # "src/signalrclient/json_hub_protocol.cpp"
        # "src/signalrclient/logger.cpp"
        # "src/signalrclient/negotiate.cpp"
        # "src/signalrclient/signalr_client_config.cpp"
        # "src/signalrclient/signalr_value.cpp"
        # "src/signalrclient/trace_log_writer.cpp"
        # "src/signalrclient/transport.cpp"
        # "src/signalrclient/transport_factory.cpp"
        # "src/signalrclient/url_builder.cpp"
        # "src/signalrclient/websocket_transport.cpp"
        # "src/signalrclient/signalr_default_scheduler.cpp"
        
        # URI utilities (to be uncommented after extraction)
        # "third_party_code/cpprestsdk/uri.cpp"
        # "third_party_code/cpprestsdk/uri_builder.cpp"
        
    INCLUDE_DIRS 
        "include"
        "third_party_code"
        
    REQUIRES 
        esp_websocket_client
        esp_http_client
        json
        freertos
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
)
