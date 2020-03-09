/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef _PXF_CURL_H_
#define _PXF_CURL_H_

#include "postgres.h"

/*
 * CHunked cURL API
 * NOTES:
 * 1) Does not multi thread
 * 2) Does not talk IPv6
 */
typedef void *PXF_CURL_HEADERS;
typedef void *PXF_CURL_HANDLE;

/*
 * Structure to store the PXF SSL options
 */
typedef struct PxfSSLOptions
{
	/* SSL options */
	bool		verbose; /* true to enable CURL's SSL library to display a lot of verbose information about its operations, false otherwise */
	bool		disable_verification;	/* true if certificate and host verification is disabled, false otherwise */
	char	   *client_cert_path;	/* full path to the cert for client authentication */
	char	   *private_key_password;	/* password for the private key file */
	char	   *private_key_path;	/* full path to the private key file */
	char	   *trusted_ca_path;	/* full path to the trusted certificate authorities file */
	int		version;	/* the SSL version (defaults to TLSv1 <TLS 1.x>) */
} PxfSSLOptions;

/*
 * PUT example
 * -----------
 *
 * PXF_CURL_HEADERS http_headers = PxfCurlHeadersInit();
 * PxfCurlHeadersAppend(http_headers, "a", "b");
 * PxfCurlHeadersAppend(http_headers, "c", "d");
 *
 * PXF_CURL_HANDLE churl = PxfCurlInitUpload("http://127.0.0.1:12345", http_headers);
 * while(have_stuff_to_write())
 * {
 *     PxfCurlWrite(churl);
 * }
 *
 * PxfCurlCleanup(churl);
 * PxfCurlHeadersCleanup(http_headers);
 *
 * GET example
 * -----------
 *
 * PXF_CURL_HEADERS http_headers = PxfCurlHeadersInit();
 * PxfCurlHeadersAppend(http_headers, "a", "b");
 * PxfCurlHeadersAppend(http_headers, "c", "d");
 *
 * PXF_CURL_HANDLE churl = PxfCurlInitDownload("http://127.0.0.1:12345", http_headers);
 *
 * char buf[64 * 1024];
 * size_t n = 0;
 * while ((n = PxfCurlRead(churl, buf, sizeof(buf))) != 0)
 * {
 *     do_something(buf, n);
 * }
 *
 * PxfCurlCleanup(churl);
 * PxfCurlHeadersCleanup(http_headers);
 */

/*
 * Create a handle for adding headers
 */
PXF_CURL_HEADERS PxfCurlHeadersInit(void);

/*
 * Add a new header
 * Headers are added in the form 'key: value'
 */
void		PxfCurlHeadersAppend(PXF_CURL_HEADERS headers, const char *key, const char *value);

/*
 * Override header with given 'key'.
 * If header doesn't exist, create new one (using PxfCurlHeadersAppend).
 * Headers are added in the form 'key: value'
 */
void		PxfCurlHeadersOverride(PXF_CURL_HEADERS headers, const char *key, const char *value);

/*
 * Remove header with given 'key'.
 * has_value specifies if the header has a value or only a key.
 * If the header doesn't exist, do nothing.
 * If the header is the first one on the list,
 * point the headers list to the next element.
 */
void		PxfCurlHeadersRemove(PXF_CURL_HEADERS headers, const char *key, bool has_value);

/*
 * Cleanup handle for headers
 */
void		PxfCurlHeadersCleanup(PXF_CURL_HEADERS headers);

/*
 * Start an upload to url
 * returns a handle to churl transfer
 */
PXF_CURL_HANDLE PxfCurlInitUpload(const char *url, PXF_CURL_HEADERS headers, PxfSSLOptions *ssl_options);

/*
 * Start a download to url
 * returns a handle to churl transfer
 */
PXF_CURL_HANDLE PxfCurlInitDownload(const char *url, PXF_CURL_HEADERS headers, PxfSSLOptions *ssl_options);

/*
 * Restart a session to a new URL
 * This will use the same headers
 */
void		PxfCurlDownloadRestart(PXF_CURL_HANDLE, const char *url, PXF_CURL_HEADERS headers);

/*
 * Send buf of bufsize
 */
size_t		PxfCurlWrite(PXF_CURL_HANDLE handle, const char *buf, size_t bufsize);

/*
 * Receive up to max_size into buf
 */
size_t		PxfCurlRead(PXF_CURL_HANDLE handle, char *buf, size_t max_size);

/*
 * Check connectivity by reading some bytes and checking response
 */
void		PxfCurlReadCheckConnectivity(PXF_CURL_HANDLE handle);

/*
 * Cleanup churl resources
 */
void		PxfCurlCleanup(PXF_CURL_HANDLE handle, bool after_error);

/*
 * Debug function - print the http headers
 */
void		PxfPrintHttpHeaders(PXF_CURL_HEADERS headers);

#define LocalhostIpV4Entry ":127.0.0.1"
#define LocalhostIpV4 "localhost"
#define REST_HEADER_JSON_RESPONSE "Accept: application/json"

#endif							/* _PXF_CURL_H_ */
