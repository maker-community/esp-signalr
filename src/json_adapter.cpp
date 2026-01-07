#include "json_adapter.h"
#include <cstring>
#include <sstream>

namespace signalr {

// ==================== json_value Implementation ====================

json_value::json_value() 
    : m_node(cJSON_CreateNull()), m_owns_node(true) {
}

json_value::json_value(cJSON* node, bool owns)
    : m_node(node), m_owns_node(owns) {
}

json_value::json_value(const json_value& other)
    : m_node(nullptr), m_owns_node(true) {
    if (other.m_node) {
        deep_copy_from(other.m_node);
    }
}

json_value& json_value::operator=(const json_value& other) {
    if (this != &other) {
        if (!m_owns_node && m_node && other.m_node) {
            // We're a non-owning wrapper (child of an object/array)
            // Need to replace the content of the cJSON node in-place
            // This is tricky with cJSON - we need to preserve the node pointer
            // but change its content
            
            char* string_backup = m_node->string;  // Preserve the key name
            m_node->string = nullptr;  // Prevent cJSON_Delete from freeing it
            
            // Delete old content
            if (m_node->child) {
                cJSON_Delete(m_node->child);
                m_node->child = nullptr;
            }
            if (m_node->valuestring) {
                cJSON_free(m_node->valuestring);
                m_node->valuestring = nullptr;
            }
            
            // Copy new content
            m_node->type = other.m_node->type;
            m_node->valueint = other.m_node->valueint;
            m_node->valuedouble = other.m_node->valuedouble;
            
            if (other.m_node->valuestring) {
                size_t len = strlen(other.m_node->valuestring);
                m_node->valuestring = (char*)cJSON_malloc(len + 1);
                if (m_node->valuestring) {
                    strcpy(m_node->valuestring, other.m_node->valuestring);
                }
            }
            
            if (other.m_node->child) {
                m_node->child = cJSON_Duplicate(other.m_node->child, 1);
            }
            
            // Restore the key name
            m_node->string = string_backup;
        } else {
            // Normal case: we own the node, so we can replace it
            release();
            if (other.m_node) {
                deep_copy_from(other.m_node);
            }
        }
    }
    return *this;
}

json_value::~json_value() {
    release();
}

void json_value::release() {
    if (m_owns_node && m_node) {
        cJSON_Delete(m_node);
        m_node = nullptr;
    }
}

void json_value::deep_copy_from(const cJSON* source) {
    if (source) {
        m_node = cJSON_Duplicate(source, 1);  // Deep copy
        m_owns_node = true;
    } else {
        m_node = nullptr;
    }
}

// Static factory methods
json_value json_value::null() {
    return json_value(cJSON_CreateNull(), true);
}

json_value json_value::object() {
    return json_value(cJSON_CreateObject(), true);
}

json_value json_value::array() {
    return json_value(cJSON_CreateArray(), true);
}

json_value json_value::from_string(const std::string& str) {
    return json_value(cJSON_CreateString(str.c_str()), true);
}

json_value json_value::from_int(int value) {
    return json_value(cJSON_CreateNumber(value), true);
}

json_value json_value::from_double(double value) {
    return json_value(cJSON_CreateNumber(value), true);
}

json_value json_value::from_bool(bool value) {
    return json_value(cJSON_CreateBool(value), true);
}

// Type queries
bool json_value::is_null() const {
    return m_node && cJSON_IsNull(m_node);
}

bool json_value::is_string() const {
    return m_node && cJSON_IsString(m_node);
}

bool json_value::is_int() const {
    return m_node && cJSON_IsNumber(m_node);
}

bool json_value::is_uint() const {
    return m_node && cJSON_IsNumber(m_node) && m_node->valuedouble >= 0;
}

bool json_value::is_double() const {
    return m_node && cJSON_IsNumber(m_node);
}

bool json_value::is_numeric() const {
    return m_node && cJSON_IsNumber(m_node);
}

bool json_value::is_bool() const {
    return m_node && cJSON_IsBool(m_node);
}

bool json_value::is_array() const {
    return m_node && cJSON_IsArray(m_node);
}

bool json_value::is_object() const {
    return m_node && cJSON_IsObject(m_node);
}

// Value extraction
std::string json_value::as_string() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsString(m_node)) {
        return m_node->valuestring ? m_node->valuestring : "";
    }
    throw std::runtime_error("JSON value is not a string");
}

int json_value::as_int() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsNumber(m_node)) {
        return static_cast<int>(m_node->valuedouble);
    }
    throw std::runtime_error("JSON value is not a number");
}

unsigned int json_value::as_uint() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsNumber(m_node)) {
        return static_cast<unsigned int>(m_node->valuedouble);
    }
    throw std::runtime_error("JSON value is not a number");
}

double json_value::as_double() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsNumber(m_node)) {
        return m_node->valuedouble;
    }
    throw std::runtime_error("JSON value is not a number");
}

bool json_value::as_bool() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsBool(m_node)) {
        return cJSON_IsTrue(m_node);
    }
    throw std::runtime_error("JSON value is not a boolean");
}

// Object/Array access operators
json_value& json_value::operator[](const char* key) {
    if (!m_node) {
        m_node = cJSON_CreateObject();
        m_owns_node = true;
    }
    
    if (!cJSON_IsObject(m_node)) {
        throw std::runtime_error("JSON value is not an object");
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(m_node, key);
    if (!item) {
        // Create new null item
        item = cJSON_CreateNull();
        cJSON_AddItemToObject(m_node, key, item);
    }
    
    // Create a non-owning wrapper for the child node
    // Important: Use a local static per-call - this is still not perfect but better
    // The real fix would require a proxy class
    static json_value temp;
    temp.release();
    temp.m_node = item;
    temp.m_owns_node = false;
    return temp;
}

json_value& json_value::operator[](const std::string& key) {
    return (*this)[key.c_str()];
}

const json_value json_value::operator[](const char* key) const {
    if (!m_node || !cJSON_IsObject(m_node)) {
        return json_value::null();
    }
    
    cJSON* item = cJSON_GetObjectItemCaseSensitive(m_node, key);
    if (!item) {
        return json_value::null();
    }
    
    return json_value(item, false);
}

const json_value json_value::operator[](const std::string& key) const {
    return (*this)[key.c_str()];
}

json_value& json_value::operator[](int index) {
    if (!m_node) {
        m_node = cJSON_CreateArray();
        m_owns_node = true;
    }
    
    if (!cJSON_IsArray(m_node)) {
        throw std::runtime_error("JSON value is not an array");
    }

    cJSON* item = cJSON_GetArrayItem(m_node, index);
    if (!item) {
        throw std::runtime_error("Array index out of bounds");
    }
    
    static json_value temp;
    temp.release();
    temp.m_node = item;
    temp.m_owns_node = false;
    return temp;
}

const json_value json_value::operator[](int index) const {
    if (!m_node || !cJSON_IsArray(m_node)) {
        return json_value::null();
    }
    
    cJSON* item = cJSON_GetArrayItem(m_node, index);
    if (!item) {
        return json_value::null();
    }
    
    return json_value(item, false);
}

// Array operations
size_t json_value::size() const {
    if (!m_node) {
        return 0;
    }
    
    if (cJSON_IsArray(m_node) || cJSON_IsObject(m_node)) {
        return cJSON_GetArraySize(m_node);
    }
    
    return 0;
}

void json_value::append(const json_value& value) {
    if (!m_node) {
        m_node = cJSON_CreateArray();
        m_owns_node = true;
    }
    
    if (!cJSON_IsArray(m_node)) {
        throw std::runtime_error("JSON value is not an array");
    }
    
    if (value.m_node) {
        cJSON* copy = cJSON_Duplicate(value.m_node, 1);
        cJSON_AddItemToArray(m_node, copy);
    }
}

// Object operations
std::vector<std::string> json_value::get_member_names() const {
    std::vector<std::string> names;
    
    if (!m_node || !cJSON_IsObject(m_node)) {
        return names;
    }
    
    cJSON* child = m_node->child;
    while (child) {
        if (child->string) {
            names.push_back(child->string);
        }
        child = child->next;
    }
    
    return names;
}

bool json_value::is_member(const std::string& key) const {
    if (!m_node || !cJSON_IsObject(m_node)) {
        return false;
    }
    
    return cJSON_GetObjectItemCaseSensitive(m_node, key.c_str()) != nullptr;
}

void json_value::remove_member(const std::string& key) {
    if (m_node && cJSON_IsObject(m_node)) {
        cJSON_DeleteItemFromObject(m_node, key.c_str());
    }
}

// Serialization
std::string json_value::to_string() const {
    if (!m_node) {
        return "null";
    }
    
    char* str = cJSON_PrintUnformatted(m_node);
    if (!str) {
        return "null";
    }
    
    std::string result(str);
    cJSON_free(str);
    return result;
}

std::string json_value::to_styled_string() const {
    if (!m_node) {
        return "null";
    }
    
    char* str = cJSON_Print(m_node);
    if (!str) {
        return "null";
    }
    
    std::string result(str);
    cJSON_free(str);
    return result;
}

// ==================== json_reader Implementation ====================

bool json_reader::parse(const std::string& document, json_value& root, bool collect_comments) {
    cJSON* parsed = cJSON_Parse(document.c_str());
    
    if (!parsed) {
        const char* error = cJSON_GetErrorPtr();
        if (error) {
            m_error_message = std::string("JSON parse error: ") + error;
        } else {
            m_error_message = "JSON parse error: Unknown error";
        }
        return false;
    }
    
    root.release();
    root.m_node = parsed;
    root.m_owns_node = true;
    
    return true;
}

std::string json_reader::get_formatted_error_messages() const {
    return m_error_message;
}

// ==================== json_stream_writer Implementation ====================

std::string json_stream_writer::write(const json_value& root) {
    return root.to_string();
}

} // namespace signalr
