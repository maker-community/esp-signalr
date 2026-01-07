// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "json_helpers.h"
#include <cmath>
#include <stdint.h>

namespace signalr
{
    char record_separator = '\x1e';

    signalr::value createValue(const json_value& v)
    {
        if (v.is_bool())
        {
            return signalr::value(v.as_bool());
        }
        else if (v.is_double())
        {
            return signalr::value(v.as_double());
        }
        else if (v.is_string())
        {
            return signalr::value(v.as_string());
        }
        else if (v.is_array())
        {
            std::vector<signalr::value> vec;
            for (size_t i = 0; i < v.size(); i++)
            {
                vec.push_back(createValue(v[i]));
            }
            return signalr::value(std::move(vec));
        }
        else if (v.is_object())
        {
            std::map<std::string, signalr::value> map;
            auto members = v.get_member_names();
            for (const auto& member : members)
            {
                map.insert({ member, createValue(v[member]) });
            }
            return signalr::value(std::move(map));
        }
        else // null or unknown
        {
            return signalr::value();
        }
    }

    char getBase64Value(uint32_t i)
    {
        char index = (char)i;
        if (index < 26)
        {
            return 'A' + index;
        }
        if (index < 52)
        {
            return 'a' + (index - 26);
        }
        if (index < 62)
        {
            return '0' + (index - 52);
        }
        if (index == 62)
        {
            return '+';
        }
        if (index == 63)
        {
            return '/';
        }

        throw std::out_of_range("base64 table index out of range: " + std::to_string(index));
    }

    std::string base64Encode(const std::vector<uint8_t>& data)
    {
        std::string base64result;

        size_t i = 0;
        while (i <= data.size() - 3)
        {
            uint32_t b = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | (uint32_t)data[i + 2];
            base64result.push_back(getBase64Value((b >> 18) & 0x3F));
            base64result.push_back(getBase64Value((b >> 12) & 0x3F));
            base64result.push_back(getBase64Value((b >> 6) & 0x3F));
            base64result.push_back(getBase64Value(b & 0x3F));

            i += 3;
        }
        if (data.size() - i == 2)
        {
            uint32_t b = ((uint32_t)data[i] << 8) | (uint32_t)data[i + 1];
            base64result.push_back(getBase64Value((b >> 10) & 0x3F));
            base64result.push_back(getBase64Value((b >> 4) & 0x3F));
            base64result.push_back(getBase64Value((b << 2) & 0x3F));
            base64result.push_back('=');
        }
        else if (data.size() - i == 1)
        {
            uint32_t b = (uint32_t)data[i];
            base64result.push_back(getBase64Value((b >> 2) & 0x3F));
            base64result.push_back(getBase64Value((b << 4) & 0x3F));
            base64result.push_back('=');
            base64result.push_back('=');
        }

        return base64result;
    }

    json_value createJson(const signalr::value& v)
    {
        switch (v.type())
        {
        case signalr::value_type::boolean:
            return json_value::from_bool(v.as_bool());
        case signalr::value_type::float64:
        {
            auto value = v.as_double();
            double intPart;
            // Workaround for 1.0 being output as 1.0 instead of 1
            // because the server expects certain values to be 1 instead of 1.0 (like protocol version)
            if (std::modf(value, &intPart) == 0)
            {
                if (value < 0)
                {
                    if (value >= (double)INT64_MIN)
                    {
                        // Fits within int64_t
                        return json_value::from_int(static_cast<int>(intPart));
                    }
                    else
                    {
                        // Remain as double
                        return json_value::from_double(value);
                    }
                }
                else
                {
                    if (value <= (double)UINT64_MAX)
                    {
                        // Fits within uint64_t
                        return json_value::from_int(static_cast<int>(intPart));
                    }
                    else
                    {
                        // Remain as double
                        return json_value::from_double(value);
                    }
                }
            }
            return json_value::from_double(v.as_double());
        }
        case signalr::value_type::string:
            return json_value::from_string(v.as_string());
        case signalr::value_type::array:
        {
            const auto& array = v.as_array();
            json_value vec = json_value::array();
            for (auto& val : array)
            {
                vec.append(createJson(val));
            }
            return vec;
        }
        case signalr::value_type::map:
        {
            const auto& obj = v.as_map();
            json_value object = json_value::object();
            for (auto& val : obj)
            {
                object[val.first] = createJson(val.second);
            }
            return object;
        }
        case signalr::value_type::binary:
        {
            const auto& binary = v.as_binary();
            return json_value::from_string(base64Encode(binary));
        }
        case signalr::value_type::null:
        default:
            return json_value::null();
        }
    }

    json_stream_writer_builder getJsonWriter()
    {
        return json_stream_writer_builder();
    }

    std::unique_ptr<json_reader> getJsonReader()
    {
        return std::unique_ptr<json_reader>(new json_reader());
    }
}