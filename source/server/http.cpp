/*
This file is part of "Rigs of Rods Server" (Relay mode)

Copyright 2007   Pierre-Michel Ricordel
Copyright 2014+  Rigs of Rods Community

"Rigs of Rods Server" is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version.

"Rigs of Rods Server" is distributed in the hope that it will
be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar. If not, see <http://www.gnu.org/licenses/>.
*/

#include "http.h"

#include "utils.h"
#include "logger.h"
#include <curl/curl.h>

#include <assert.h>
#include <vector>
#include <cstring>

namespace {

size_t CurlWriteCallback(char *data, size_t size, size_t count, void *userdata)
{
    const size_t bytes = size * count;
    static_cast<std::string *>(userdata)->append(data, bytes);
    return bytes;
}

bool AppendHeader(curl_slist *&headers, const char *value)
{
    curl_slist *updated = curl_slist_append(headers, value);
    if (updated == nullptr)
    {
        return false;
    }
    headers = updated;
    return true;
}

} // namespace

namespace Http {

    const char *METHOD_GET = "GET";
    const char *METHOD_POST = "POST";
    const char *METHOD_PUT = "PUT";
    const char *METHOD_DELETE = "DELETE";

    bool Initialize()
    {
        CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK)
        {
            Logger::Log(LOG_ERROR, "Unable to initialize bounded HTTP transport: %s", curl_easy_strerror(result));
            return false;
        }
        return true;
    }


    std::string RequestRaw(
            std::string method,
            std::string host,
            std::string url,
            std::string content_type,
            std::string payload) {
        method = method.empty() ? METHOD_GET : method;

        CURL *curl = curl_easy_init();
        if (curl == nullptr) {
            Logger::Log(LOG_ERROR, "Could not initialize an HTTP request");
            return "";
        }

        curl_slist *headers = nullptr;
        std::string content_type_header = "Content-Type: " + content_type;
        if (!AppendHeader(headers, content_type_header.c_str()) ||
            !AppendHeader(headers, "Expect:")) {
            Logger::Log(LOG_ERROR, "Could not allocate HTTP request headers");
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return "";
        }

        std::string response;
        std::string request_url = "http://" + host + url;
        char error_buffer[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTP_TRANSFER_DECODING, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

        CURLcode result = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (result != CURLE_OK) {
            const char *detail = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(result);
            Logger::Log(LOG_ERROR, "HTTP %s transport failed: %s", method.c_str(), detail);
            return "";
        }

        return response;
    }

    int Request(
            std::string method,
            std::string host,
            std::string url,
            std::string content_type,
            std::string payload,
            Response *response) {
        assert(response != nullptr);

        std::string response_str = RequestRaw(method, host, url, content_type, payload);

        if (response_str.empty()) {
            return -1;
        }

        if (!response->FromBuffer(response_str)) {
            return -2;
        }

        return response->GetCode();
    }

    Response::Response() :
            m_response_code(0) {
    }

    const std::string &Response::GetBody() {
        return m_headermap["body"];
    }

    const std::vector<std::string> Response::GetBodyLines() {
        std::vector<std::string> lines;
        strict_tokenize(m_headermap["body"], lines, "\n");
        return lines;
    }

    bool Response::IsChunked() {
        return "chunked" == m_headermap["Transfer-Encoding"];
    }

    bool Response::FromBuffer(const std::string &message) {
        m_response_code = -1;
        m_headermap.clear();

        std::size_t locHolder;
        locHolder = message.find("\r\n\r\n");
        std::vector<std::string> header;
        std::vector<std::string> tmp;

        strict_tokenize(message.substr(0, locHolder), header, "\r\n");

        // Process response code. The input has form "HTTP/1.1 200 OK"
        char line[200];
        strcpy(line, header[0].c_str());
        char *tok0 = std::strtok(line, " ");
        char *tok1 = std::strtok(nullptr, " ");
        if (tok0 == nullptr || tok1 == nullptr) {
            Logger::Log(LOG_ERROR, "Internal: HTTP response has malformed 1st line: \n%s", header[0].c_str());
            return false;
        }
        m_response_code = atoi(tok1);

        // Process header lines
        for (unsigned short index = 1; index < header.size(); index++) {
            tmp.clear();
            tokenize(header[index], tmp, ":");
            if (tmp.size() != 2) {
                continue;
            }
            m_headermap[trim(tmp[0])] = trim(tmp[1]);
        }

        tmp.clear();
        locHolder = message.find_first_not_of("\r\n", locHolder);
        if (std::string::npos == locHolder) {
            Logger::Log(LOG_ERROR, "Internal: HTTP message does not appear to contain a body: \n%s", message.c_str());
            return false;
        }

        strict_tokenize(message.substr(locHolder), tmp, "\r\n");
        if (IsChunked()) {
            m_headermap["body"] = tmp[1];
        } else {
            m_headermap["body"] = tmp[0];
        }
        return true;
    }

} // namespace Http
