#include "json_adapter.h"
#include <cstring>
#include <sstream>
#include "esp_log.h"

static const char* JSON_ADAPTER_TAG = "JSON_ADAPTER";

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

json_value::json_value(json_value&& other) noexcept
    : m_node(other.m_node), m_owns_node(other.m_owns_node) {
    other.m_node = nullptr;
    other.m_owns_node = false;
}

json_value& json_value::operator=(json_value&& other) noexcept {
    if (this != &other) {
        if (!m_owns_node && m_node && other.m_node) {
            // We're a non-owning wrapper (child of an object/array)
            // Need to MOVE the content into the existing node structure
            
            char* string_backup = m_node->string;  // Preserve the key name
            m_node->string = nullptr;
            
            // Delete old content
            if (m_node->child) {
                cJSON_Delete(m_node->child);
                m_node->child = nullptr;
            }
            if (m_node->valuestring) {
                cJSON_free(m_node->valuestring);
                m_node->valuestring = nullptr;
            }
            
            // MOVE content from other (no copy!)
            m_node->type = other.m_node->type;
            m_node->valueint = other.m_node->valueint;
            m_node->valuedouble = other.m_node->valuedouble;
            
            // Transfer ownership of string (no copy!)
            m_node->valuestring = other.m_node->valuestring;
            other.m_node->valuestring = nullptr;
            
            // Transfer ownership of children (no copy!)
            m_node->child = other.m_node->child;
            other.m_node->child = nullptr;
            
            // Restore the key name
            m_node->string = string_backup;
            
            // Clean up the source node (it's now empty)
            if (other.m_owns_node) {
                cJSON_Delete(other.m_node);
            }
            other.m_node = nullptr;
            other.m_owns_node = false;
        } else {
            // Normal case: we own the node, so we can just swap
            release();
            m_node = other.m_node;
            m_owns_node = other.m_owns_node;
            other.m_node = nullptr;
            other.m_owns_node = false;
        }
    }
    return *this;
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
            
            // For arrays and objects, we need to duplicate ALL children (including siblings)
            // cJSON_Duplicate with recurse=1 only copies children recursively, not siblings
            // So for array/object, we need to duplicate the entire children chain
            if (other.m_node->child) {
                // Duplicate the entire children linked list
                cJSON* src_child = other.m_node->child;
                cJSON* first_copy = nullptr;
                cJSON* prev_copy = nullptr;
                
                while (src_child) {
                    cJSON* copy = cJSON_Duplicate(src_child, 1);  // Deep copy this child
                    if (copy) {
                        if (!first_copy) {
                            first_copy = copy;
                        }
                        if (prev_copy) {
                            prev_copy->next = copy;
                            copy->prev = prev_copy;
                        }
                        prev_copy = copy;
                    }
                    src_child = src_child->next;
                }
                
                m_node->child = first_copy;
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
    cJSON* node = cJSON_CreateObject();
    if (!node) {
        ESP_LOGE(JSON_ADAPTER_TAG, "Failed to create JSON object - OUT OF MEMORY!");
        throw std::runtime_error("Out of memory: failed to create JSON object");
    }
    return json_value(node, true);
}

json_value json_value::array() {
    cJSON* node = cJSON_CreateArray();
    if (!node) {
        ESP_LOGE(JSON_ADAPTER_TAG, "Failed to create JSON array - OUT OF MEMORY!");
        throw std::runtime_error("Out of memory: failed to create JSON array");
    }
    return json_value(node, true);
}

json_value json_value::from_string(const std::string& str) {
    cJSON* node = cJSON_CreateString(str.c_str());
    if (!node) {
        ESP_LOGE(JSON_ADAPTER_TAG, "Failed to create JSON string! Length: %d bytes - OUT OF MEMORY!", str.length());
        // Don't return null - throw exception to prevent sending invalid JSON to server
        throw std::runtime_error("Out of memory: failed to create JSON string");
    }
    return json_value(node, true);
}

json_value json_value::from_int(int value) {
    cJSON* node = cJSON_CreateNumber(value);
    if (!node) {
        ESP_LOGE(JSON_ADAPTER_TAG, "Failed to create JSON number - OUT OF MEMORY!");
        throw std::runtime_error("Out of memory: failed to create JSON number");
    }
    return json_value(node, true);
}

json_value json_value::from_double(double value) {
    cJSON* node = cJSON_CreateNumber(value);
    if (!node) {
        ESP_LOGE(JSON_ADAPTER_TAG, "Failed to create JSON number - OUT OF MEMORY!");
        throw std::runtime_error("Out of memory: failed to create JSON number");
    }
    return json_value(node, true);
}

json_value json_value::from_bool(bool value) {
    cJSON* node = cJSON_CreateBool(value);
    if (!node) {
        ESP_LOGE(JSON_ADAPTER_TAG, "Failed to create JSON bool - OUT OF MEMORY!");
        throw std::runtime_error("Out of memory: failed to create JSON bool");
    }
    return json_value(node, true);
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

// Object/Array access operators - using proxy class to avoid static variable issues
json_value_proxy json_value::operator[](const char* key) {
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
    
    return json_value_proxy(m_node, item);
}

json_value_proxy json_value::operator[](const std::string& key) {
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

json_value_proxy json_value::operator[](int index) {
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
    
    return json_value_proxy(m_node, item);
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
        if (!copy) {
            ESP_LOGE(JSON_ADAPTER_TAG, "Failed to duplicate JSON value for append! Type: %d", value.m_node->type);
            // Still add a null placeholder to maintain array indices
            copy = cJSON_CreateNull();
            if (copy) {
                cJSON_AddItemToArray(m_node, copy);
            }
            return;
        }
        
        cJSON_AddItemToArray(m_node, copy);
    } else {
        cJSON* null_item = cJSON_CreateNull();
        if (null_item) {
            cJSON_AddItemToArray(m_node, null_item);
        }
    }
}

// Move-semantic append - transfers ownership instead of copying
// This is crucial for large strings (like base64 audio data) to avoid memory allocation failures
void json_value::append(json_value&& value) {
    if (!m_node) {
        m_node = cJSON_CreateArray();
        m_owns_node = true;
    }
    
    if (!cJSON_IsArray(m_node)) {
        throw std::runtime_error("JSON value is not an array");
    }
    
    if (value.m_node) {
        if (value.m_owns_node) {
            // Transfer ownership - no memory allocation needed!
            cJSON_AddItemToArray(m_node, value.m_node);
            value.m_node = nullptr;  // Prevent destruction
            value.m_owns_node = false;
        } else {
            // Non-owning - must copy
            cJSON* copy = cJSON_Duplicate(value.m_node, 1);
            if (copy) {
                cJSON_AddItemToArray(m_node, copy);
            } else {
                ESP_LOGE(JSON_ADAPTER_TAG, "Failed to duplicate non-owned JSON value for move-append!");
                cJSON* null_item = cJSON_CreateNull();
                if (null_item) {
                    cJSON_AddItemToArray(m_node, null_item);
                }
            }
        }
    } else {
        cJSON* null_item = cJSON_CreateNull();
        if (null_item) {
            cJSON_AddItemToArray(m_node, null_item);
        }
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
        ESP_LOGE(JSON_ADAPTER_TAG, "Cannot serialize null JSON node");
        throw std::runtime_error("Cannot serialize null JSON node");
    }
    
    char* str = cJSON_PrintUnformatted(m_node);
    if (!str) {
        ESP_LOGE(JSON_ADAPTER_TAG, "cJSON_PrintUnformatted failed - OUT OF MEMORY or invalid JSON structure");
        throw std::runtime_error("JSON serialization failed: out of memory or invalid structure");
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

// ==================== json_value_proxy Implementation ====================

json_value_proxy::json_value_proxy(cJSON* parent, cJSON* node)
    : m_parent(parent), m_node(node) {
}

json_value_proxy& json_value_proxy::operator=(const json_value& value) {
    if (!m_node || !value.m_node) {
        return *this;
    }
    
    // Preserve the key name
    char* string_backup = m_node->string;
    m_node->string = nullptr;
    
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
    m_node->type = value.m_node->type;
    m_node->valueint = value.m_node->valueint;
    m_node->valuedouble = value.m_node->valuedouble;
    
    // Copy string if present
    if (value.m_node->valuestring) {
        size_t len = strlen(value.m_node->valuestring);
        m_node->valuestring = (char*)cJSON_malloc(len + 1);
        if (m_node->valuestring) {
            strcpy(m_node->valuestring, value.m_node->valuestring);
        }
    }
    
    // Deep copy children for arrays/objects
    if (value.m_node->child) {
        cJSON* src_child = value.m_node->child;
        cJSON* first_copy = nullptr;
        cJSON* prev_copy = nullptr;
        
        while (src_child) {
            cJSON* copy = cJSON_Duplicate(src_child, 1);
            if (copy) {
                if (!first_copy) {
                    first_copy = copy;
                }
                if (prev_copy) {
                    prev_copy->next = copy;
                    copy->prev = prev_copy;
                }
                prev_copy = copy;
            }
            src_child = src_child->next;
        }
        
        m_node->child = first_copy;
    }
    
    // Restore the key name
    m_node->string = string_backup;
    
    return *this;
}

json_value_proxy& json_value_proxy::operator=(json_value&& value) {
    if (!m_node || !value.m_node) {
        return *this;
    }
    
    // Preserve the key name
    char* string_backup = m_node->string;
    m_node->string = nullptr;
    
    // Delete old content
    if (m_node->child) {
        cJSON_Delete(m_node->child);
        m_node->child = nullptr;
    }
    if (m_node->valuestring) {
        cJSON_free(m_node->valuestring);
        m_node->valuestring = nullptr;
    }
    
    // MOVE content (no copy!)
    m_node->type = value.m_node->type;
    m_node->valueint = value.m_node->valueint;
    m_node->valuedouble = value.m_node->valuedouble;
    
    // Transfer ownership of string (no copy!)
    m_node->valuestring = value.m_node->valuestring;
    value.m_node->valuestring = nullptr;
    
    // Transfer ownership of children (no copy!)
    m_node->child = value.m_node->child;
    value.m_node->child = nullptr;
    
    // Restore the key name
    m_node->string = string_backup;
    
    // Clean up the source node (it's now empty)
    if (value.m_owns_node && value.m_node) {
        cJSON_Delete(value.m_node);
        value.m_node = nullptr;
        value.m_owns_node = false;
    }
    
    return *this;
}

json_value_proxy::operator json_value() const {
    // Return a non-owning json_value wrapper
    return json_value(m_node, false);
}

json_value_proxy json_value_proxy::operator[](const char* key) {
    if (!m_node || !cJSON_IsObject(m_node)) {
        throw std::runtime_error("JSON value is not an object");
    }
    
    cJSON* item = cJSON_GetObjectItemCaseSensitive(m_node, key);
    if (!item) {
        item = cJSON_CreateNull();
        cJSON_AddItemToObject(m_node, key, item);
    }
    
    return json_value_proxy(m_node, item);
}

json_value_proxy json_value_proxy::operator[](const std::string& key) {
    return (*this)[key.c_str()];
}

json_value_proxy json_value_proxy::operator[](int index) {
    if (!m_node || !cJSON_IsArray(m_node)) {
        throw std::runtime_error("JSON value is not an array");
    }
    
    cJSON* item = cJSON_GetArrayItem(m_node, index);
    if (!item) {
        throw std::runtime_error("Array index out of bounds");
    }
    
    return json_value_proxy(m_node, item);
}

// Value extraction methods
std::string json_value_proxy::as_string() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsString(m_node)) {
        return m_node->valuestring ? m_node->valuestring : "";
    }
    throw std::runtime_error("JSON value is not a string");
}

int json_value_proxy::as_int() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsNumber(m_node)) {
        return static_cast<int>(m_node->valuedouble);
    }
    throw std::runtime_error("JSON value is not a number");
}

unsigned int json_value_proxy::as_uint() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsNumber(m_node)) {
        return static_cast<unsigned int>(m_node->valuedouble);
    }
    throw std::runtime_error("JSON value is not a number");
}

double json_value_proxy::as_double() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsNumber(m_node)) {
        return m_node->valuedouble;
    }
    throw std::runtime_error("JSON value is not a number");
}

bool json_value_proxy::as_bool() const {
    if (!m_node) {
        throw std::runtime_error("Null JSON value");
    }
    if (cJSON_IsBool(m_node)) {
        return cJSON_IsTrue(m_node);
    }
    throw std::runtime_error("JSON value is not a boolean");
}

// Type queries
bool json_value_proxy::is_null() const {
    return m_node && cJSON_IsNull(m_node);
}

bool json_value_proxy::is_string() const {
    return m_node && cJSON_IsString(m_node);
}

bool json_value_proxy::is_array() const {
    return m_node && cJSON_IsArray(m_node);
}

bool json_value_proxy::is_object() const {
    return m_node && cJSON_IsObject(m_node);
}

size_t json_value_proxy::size() const {
    if (!m_node) {
        return 0;
    }
    if (cJSON_IsArray(m_node) || cJSON_IsObject(m_node)) {
        return cJSON_GetArraySize(m_node);
    }
    return 0;
}

// Const versions of operator[] for chained access
json_value_proxy json_value_proxy::operator[](const char* key) const {
    if (!m_node || !cJSON_IsObject(m_node)) {
        throw std::runtime_error("JSON value is not an object");
    }
    
    cJSON* item = cJSON_GetObjectItemCaseSensitive(m_node, key);
    if (!item) {
        // For const access, we can't create new items, but return a proxy to null
        static cJSON null_node;
        null_node.type = cJSON_NULL;
        return json_value_proxy(const_cast<cJSON*>(m_node), &null_node);
    }
    
    return json_value_proxy(const_cast<cJSON*>(m_node), item);
}

json_value_proxy json_value_proxy::operator[](const std::string& key) const {
    return (*this)[key.c_str()];
}

json_value_proxy json_value_proxy::operator[](int index) const {
    if (!m_node || !cJSON_IsArray(m_node)) {
        throw std::runtime_error("JSON value is not an array");
    }
    
    cJSON* item = cJSON_GetArrayItem(m_node, index);
    if (!item) {
        throw std::runtime_error("Array index out of bounds");
    }
    
    return json_value_proxy(const_cast<cJSON*>(m_node), item);
}

} // namespace signalr
