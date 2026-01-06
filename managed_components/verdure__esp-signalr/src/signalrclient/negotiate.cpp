// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "stdafx.h"
#include "negotiate.h"
#include "url_builder.h"
#include "signalrclient/signalr_exception.h"
#include "json_helpers.h"
#include "cancellation_token_source.h"

namespace signalr
{
    namespace negotiate
    {
        const int negotiate_version = 1;

        void negotiate(std::shared_ptr<http_client> client, const std::string& base_url,
            const signalr_client_config& config,
            std::function<void(negotiation_response&&, std::exception_ptr)> callback, cancellation_token token) noexcept
        {
            std::string negotiate_url;
            try
            {
                negotiate_url = url_builder::build_negotiate(base_url);
                negotiate_url = url_builder::add_query_string(negotiate_url, "negotiateVersion=" + std::to_string(negotiate_version));
            }
            catch (...)
            {
                callback({}, std::current_exception());
                return;
            }

            // TODO: signalr_client_config
            http_request request;
            request.method = http_method::POST;
            request.headers = config.get_http_headers();
#ifdef USE_CPPRESTSDK
            request.timeout = config.get_http_client_config().timeout();
#endif

            client->send(negotiate_url, request, [callback, token](const http_response& http_response, std::exception_ptr exception)
            {
                if (exception != nullptr)
                {
                    callback({}, exception);
                    return;
                }

                if (token.is_canceled())
                {
                    callback({}, std::make_exception_ptr(canceled_exception()));
                    return;
                }

                if (http_response.status_code != 200)
                {
                    callback({}, std::make_exception_ptr(
                        signalr_exception("negotiate failed with status code " + std::to_string(http_response.status_code))));
                    return;
                }

                try
                {
                    json_value negotiation_response_json;
                    auto reader = getJsonReader();

                    if (!reader->parse(http_response.content, negotiation_response_json))
                    {
                        throw signalr_exception(reader->get_formatted_error_messages());
                    }

                    negotiation_response response;

                    if (negotiation_response_json.is_member("error"))
                    {
                        response.error = negotiation_response_json["error"].as_string();
                        callback(std::move(response), nullptr);
                        return;
                    }

                    int server_negotiate_version = 0;
                    if (negotiation_response_json.is_member("negotiateVersion"))
                    {
                        server_negotiate_version = negotiation_response_json["negotiateVersion"].as_int();
                    }

                    if (negotiation_response_json.is_member("connectionId"))
                    {
                        response.connectionId = negotiation_response_json["connectionId"].as_string();
                    }

                    if (negotiation_response_json.is_member("connectionToken"))
                    {
                        response.connectionToken = negotiation_response_json["connectionToken"].as_string();
                    }

                    if (server_negotiate_version <= 0)
                    {
                        response.connectionToken = response.connectionId;
                    }

                    if (negotiation_response_json.is_member("availableTransports"))
                    {
                        const auto& transports = negotiation_response_json["availableTransports"];
                        for (size_t i = 0; i < transports.size(); i++)
                        {
                            const auto& transportData = transports[i];
                            available_transport transport;
                            transport.transport = transportData["transport"].as_string();

                            const auto& formats = transportData["transferFormats"];
                            for (size_t j = 0; j < formats.size(); j++)
                            {
                                transport.transfer_formats.push_back(formats[j].as_string());
                            }

                            response.availableTransports.push_back(transport);
                        }
                    }

                    if (negotiation_response_json.is_member("url"))
                    {
                        response.url = negotiation_response_json["url"].as_string();

                        if (negotiation_response_json.is_member("accessToken"))
                        {
                            response.accessToken = negotiation_response_json["accessToken"].as_string();
                        }
                    }

                    if (negotiation_response_json.is_member("ProtocolVersion"))
                    {
                        callback({}, std::make_exception_ptr(
                            signalr_exception("Detected a connection attempt to an ASP.NET SignalR Server. This client only supports connecting to an ASP.NET Core SignalR Server. See https://aka.ms/signalr-core-differences for details.")));
                        return;
                    }

                    callback(std::move(response), nullptr);
                }
                catch (...)
                {
                    callback({}, std::current_exception());
                }
            }, token);
        }
    }
}
