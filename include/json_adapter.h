#pragma once

#include "cJSON.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <memory>

namespace signalr {

// Forward declaration
class json_value;

/**
 * Proxy class for json_value::operator[] to avoid static variable issues
 * This allows safe assignment to object members without memory corruption
 */
class json_value_proxy {
public:
    json_value_proxy(cJSON* parent, cJSON* node);
    
    // Assignment operators - these modify the actual cJSON node
    json_value_proxy& operator=(const json_value& value);
    json_value_proxy& operator=(json_value&& value);
    
    // Conversion to json_value for reading
    operator json_value() const;
    
    // Allow chained access (const and non-const versions)
    json_value_proxy operator[](const char* key);
    json_value_proxy operator[](const std::string& key);
    json_value_proxy operator[](int index);
    json_value_proxy operator[](const char* key) const;
    json_value_proxy operator[](const std::string& key) const;
    json_value_proxy operator[](int index) const;
    
    // Value extraction methods (delegate to underlying node)
    std::string as_string() const;
    int as_int() const;
    unsigned int as_uint() const;
    double as_double() const;
    bool as_bool() const;
    
    // Type queries
    bool is_null() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;
    
    // Array/Object size
    size_t size() const;
    
private:
    cJSON* m_parent;  // Parent object/array node
    cJSON* m_node;    // The actual child node
};

/**
 * JSON adapter wrapping cJSON to provide jsoncpp-compatible API
 * This allows SignalR core code to use cJSON with minimal changes
 */
class json_value {
public:
    // Constructors
    json_value();
    json_value(const json_value& other);
    json_value(json_value&& other) noexcept;  // Move constructor
    json_value& operator=(const json_value& other);
    json_value& operator=(json_value&& other) noexcept;  // Move assignment
    ~json_value();

    // Type constructors
    static json_value null();
    static json_value object();
    static json_value array();
    static json_value from_string(const std::string& str);
    static json_value from_int(int value);
    static json_value from_double(double value);
    static json_value from_bool(bool value);

    // Type queries
    bool is_null() const;
    bool is_string() const;
    bool is_int() const;
    bool is_uint() const;
    bool is_double() const;
    bool is_numeric() const;
    bool is_bool() const;
    bool is_array() const;
    bool is_object() const;

    // Value extraction
    std::string as_string() const;
    int as_int() const;
    unsigned int as_uint() const;
    double as_double() const;
    bool as_bool() const;

    // Object/Array operations - return proxy for safe assignment
    json_value_proxy operator[](const char* key);
    json_value_proxy operator[](const std::string& key);
    const json_value operator[](const char* key) const;
    const json_value operator[](const std::string& key) const;
    
    json_value_proxy operator[](int index);
    const json_value operator[](int index) const;

    // Array operations
    size_t size() const;
    void append(const json_value& value);
    void append(json_value&& value);  // Move-semantic append to avoid copying large values
    
    // Object operations
    std::vector<std::string> get_member_names() const;
    bool is_member(const std::string& key) const;
    void remove_member(const std::string& key);

    // Serialization
    std::string to_string() const;
    std::string to_styled_string() const;

private:
    // Internal constructor from cJSON*
    explicit json_value(cJSON* node, bool owns = true);
    
    void deep_copy_from(const cJSON* source);
    void release();

    cJSON* m_node;
    bool m_owns_node;

    friend class json_reader;
    friend class json_writer;
    friend class json_value_proxy;
};

/**
 * JSON parser
 */
class json_reader {
public:
    json_reader() = default;
    
    bool parse(const std::string& document, json_value& root, bool collect_comments = true);
    std::string get_formatted_error_messages() const;

private:
    std::string m_error_message;
};

/**
 * JSON writer
 */
class json_writer {
public:
    virtual ~json_writer() = default;
    virtual std::string write(const json_value& root) = 0;
};

class json_stream_writer : public json_writer {
public:
    json_stream_writer() = default;
    std::string write(const json_value& root) override;
};

/**
 * JSON writer builder (compatibility with jsoncpp API)
 */
class json_stream_writer_builder {
public:
    json_stream_writer_builder() = default;
    
    std::unique_ptr<json_stream_writer> new_stream_writer() const {
        return std::unique_ptr<json_stream_writer>(new json_stream_writer());
    }
    
    std::string write(const json_value& root) const {
        json_stream_writer writer;
        return writer.write(root);
    }
};

// Convenience function to parse JSON
inline json_value parse_json(const std::string& json_str) {
    json_reader reader;
    json_value root;
    if (!reader.parse(json_str, root)) {
        throw std::runtime_error("JSON parse error: " + reader.get_formatted_error_messages());
    }
    return root;
}

} // namespace signalr
