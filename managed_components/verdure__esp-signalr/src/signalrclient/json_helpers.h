// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include "signalrclient/signalr_value.h"
#include "signalrclient/json_adapter.h"
#include <memory>

namespace signalr
{
    extern char record_separator;

    signalr::value createValue(const json_value& v);

    json_value createJson(const signalr::value& v);

    std::string base64Encode(const std::vector<uint8_t>& data);

    json_stream_writer_builder getJsonWriter();
    std::unique_ptr<json_reader> getJsonReader();
}