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

#include "pxf_curl.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"

/* include libcurl without typecheck.
 * This allows wrapping curl_easy_setopt to be wrapped
 * for readability. O/w an error is generated when anything
 * other than the expected type is given as parameter
 */
#define CURL_DISABLE_TYPECHECK
#include <curl/curl.h>
#undef CURL_DISABLE_TYPECHECK

/*
 * internal buffer for libchurl internal context
 */
typedef struct curl_buffer
{
	char		*ptr;
	int			max;
	int		bot,
				top;

} curl_buffer;

/*
 * internal context of libchurl
 */
typedef struct curl_context
{
	/* curl easy API handle */
	CURL	   *curl_handle;

	/*
	 * curl multi API handle used to allow non-blocking callbacks
	 */
	CURLM	   *multi_handle;

	/*
	 * curl API puts internal errors in this buffer used for error reporting
	 */
	char		curl_error_buffer[CURL_ERROR_SIZE];

	/*
	 * perform() (libcurl API) lets us know if the session is over using this
	 * int
	 */
	int			curl_still_running;

	/* internal buffer for download */
	curl_buffer *download_buffer;

	/* internal buffer for upload */
	curl_buffer *upload_buffer;

	/*
	 * holds http error code returned from remote server
	 */
	char	   *last_http_reponse;

	/* true on upload, false on download */
	bool		upload;
} curl_context;

/*
 * holds http header properties
 */
typedef struct churl_settings
{
	struct curl_slist *headers;
} churl_settings;

#define SSL_NO_VERIFY	0L
#define SSL_VERIFYPEER	1L
#define SSL_VERIFYHOST	2L
#define PROTOCOL_HTTPS	"https://"

#define IS_HTTPS_URI(uri_str) (pg_strncasecmp(uri_str, PROTOCOL_HTTPS, strlen(PROTOCOL_HTTPS)) == 0)

static curl_context	*CurlNewContext(void);
static void	CreateCurlHandle(curl_context *context);
static void	SetCurlOption(curl_context *context, CURLoption option, const void *data);
static size_t	ReadCallback(void *ptr, size_t size, size_t nmemb, void *userdata);
static void	SetupMultiHandle(curl_context *context);
static void	MultiPerform(curl_context *context);
static bool	InternalBufferLargeEnough(curl_buffer *buffer, size_t required);
static void	FlushInternalBuffer(curl_context *context);
static char	*GetDestAddress(CURL * curl_handle);
static void	EnlargeInternalBuffer(curl_buffer *buffer, size_t required);
static void	FinishUpload(curl_context *context);
static void	CleanupCurlHandle(curl_context *context);
static void	MultiRemoveHandle(curl_context *context);
static void	CleanupInternalBuffer(curl_buffer *buffer);
static void	CurlCleanupContext(curl_context *context);
static size_t	WriteCallback(char *buffer, size_t size, size_t nitems, void *userp);
static void	FillInternalBuffer(curl_context *context, int want);
static void	CurlHeadersSet(curl_context *context, PXF_CURL_HEADERS settings);
static void	CheckResponseStatus(curl_context *context);
static void	CheckResponseCode(curl_context *context);
static void	CheckResponse(curl_context *context);
static void	ClearErrorBuffer(curl_context *context);
static size_t	HeaderCallback(char *buffer, size_t size, size_t nitems, void *userp);
static void	FreeHttpResponse(curl_context *context);
static void	CompactInternalBuffer(curl_buffer *buffer);
static void	ReallocInternalBuffer(curl_buffer *buffer, size_t required);
static bool	HandleSpecialError(long response, StringInfo err);
static char	*GetHttpErrorMsg(long http_ret_code, char *msg, char *curl_error_buffer, char **trace_message);
static char	*BuildHeaderStr(const char *format, const char *key, const char *value);
static bool	FileExistsAndCanRead(char *filename);

/*
 * Debug function - print the http headers
 */
void
PxfPrintHttpHeaders(PXF_CURL_HEADERS headers)
{
	if ((DEBUG2 >= log_min_messages) || (DEBUG2 >= client_min_messages))
	{
		churl_settings *settings = (churl_settings *) headers;
		struct curl_slist *header_cell = settings->headers;
		char	   *header_data;
		int			count = 0;

		while (header_cell != NULL)
		{
			header_data = header_cell->data;
			elog(DEBUG2, "churl http header: cell #%d: %s",
				 count, header_data ? header_data : "NONE");
			header_cell = header_cell->next;
			++count;
		}
	}
}

PXF_CURL_HEADERS
PxfCurlHeadersInit(void)
{
	churl_settings *settings = (churl_settings *) palloc0(sizeof(churl_settings));

	return (PXF_CURL_HEADERS) settings;
}

/*
 * Build a header string, in the form of given format (e.g. "%s: %s"),
 * and populate <key> and <value> in it.
 * If value is empty, return <key>.
 */
static char *
BuildHeaderStr(const char *format, const char *key, const char *value)
{
	char	   *output = NULL;
	char	   *header_option = NULL;

	if (value == NULL)			/* the option is just a "key" */
		header_option = pstrdup(key);
	else						/* the option is a "key: value" */
	{
		StringInfoData formatter;

		initStringInfo(&formatter);

		/* Only encode custom headers */
		if (pg_strncasecmp("X-GP-", key, 5) == 0)
		{
			output = curl_easy_escape(NULL, value, strlen(value));

			if (!output)
				elog(ERROR, "internal error: curl_easy_escape failed for value %s", value);

			appendStringInfo(&formatter, format, key, output);
			curl_free(output);
		}
		else
		{
			appendStringInfo(&formatter, format, key, value);
		}

		header_option = formatter.data;
	}
	return header_option;
}

void
PxfCurlHeadersAppend(PXF_CURL_HEADERS headers, const char *key, const char *value)
{
	churl_settings *settings = (churl_settings *) headers;
	char	   *header_option = NULL;

	header_option = BuildHeaderStr("%s: %s", key, value);

	settings->headers = curl_slist_append(settings->headers,
										  header_option);
	pfree(header_option);
}

void
PxfCurlHeadersOverride(PXF_CURL_HEADERS headers, const char *key, const char *value)
{

	churl_settings *settings = (churl_settings *) headers;
	struct curl_slist *header_cell = settings->headers;
	char	   *key_option = NULL;
	char	   *header_data = NULL;

	/* key must not be empty */
	Assert(key != NULL);

	/* key to compare with in the headers */
	key_option = BuildHeaderStr("%s:%s", key, value ? "" : NULL);

	/* find key in headers list */
	while (header_cell != NULL)
	{
		header_data = header_cell->data;

		if (strncmp(key_option, header_data, strlen(key_option)) == 0)
		{
			elog(DEBUG2, "PxfCurlHeadersOverride: Found existing header %s with key %s (for new value %s)",
				 header_data, key_option, value);
			break;
		}
		header_cell = header_cell->next;
	}

	if (header_cell != NULL)	/* found key */
	{
		char	   *new_data = BuildHeaderStr("%s: %s", key, value);
		char	   *old_data = header_cell->data;

		header_cell->data = strdup(new_data);
		elog(DEBUG4, "PxfCurlHeadersOverride: new data: %s, old data: %s", new_data, old_data);
		free(old_data);
		pfree(new_data);
	}
	else
	{
		PxfCurlHeadersAppend(headers, key, value);
	}

	pfree(key_option);
}

void
PxfCurlHeadersRemove(PXF_CURL_HEADERS headers, const char *key, bool has_value)
{

	churl_settings *settings = (churl_settings *) headers;
	struct curl_slist *to_del_cell = settings->headers;
	struct curl_slist *prev_cell = NULL;
	char	   *key_option = NULL;
	char	   *header_data = NULL;

	/* key must not be empty */
	Assert(key != NULL);

	/* key to compare with in the headers */
	key_option = BuildHeaderStr("%s:%s", key, has_value ? "" : NULL);

	/* find key in headers list */
	while (to_del_cell != NULL)
	{

		header_data = to_del_cell->data;

		if (strncmp(key_option, header_data, strlen(key_option)) == 0)
		{
			elog(DEBUG2, "PxfCurlHeadersRemove: Found existing header %s with key %s",
				 header_data, key_option);
			break;
		}
		prev_cell = to_del_cell;
		to_del_cell = to_del_cell->next;
	}

	if (to_del_cell != NULL)	/* found key */
	{
		/* skip this cell */
		if (prev_cell != NULL)
		{
			/* not the header */
			prev_cell->next = to_del_cell->next;
		}
		else
		{
			/* remove header - make the next cell header now */
			settings->headers = to_del_cell->next;
		}

		/* remove header data and cell */
		if (to_del_cell->data)
			free(to_del_cell->data);
		free(to_del_cell);
	}
	else
	{
		elog(DEBUG2, "PxfCurlHeadersRemove: No header with key %s to remove",
			 key_option);
	}

	pfree(key_option);
}

void
PxfCurlHeadersCleanup(PXF_CURL_HEADERS headers)
{
	churl_settings *settings = (churl_settings *) headers;

	if (!settings)
		return;

	if (settings->headers)
		curl_slist_free_all(settings->headers);

	pfree(settings);
}

static PXF_CURL_HANDLE
PxfCurlInit(const char *url, PXF_CURL_HEADERS headers, PxfSSLOptions *ssl_options)
{
	int		curl_error;
	curl_context *context = CurlNewContext();

	CreateCurlHandle(context);
	ClearErrorBuffer(context);

/* Required for resolving localhost on some docker environments that
 * had intermittent networking issues when using pxf on HAWQ
 * However, CURLOPT_RESOLVE is only available in curl versions 7.21 and above */
#ifdef CURLOPT_RESOLVE
	if (strstr(url, LocalhostIpV4) != NULL)
	{
		struct curl_slist *resolve_hosts = NULL;
		char	   *pxf_host_entry = (char *) palloc0(strlen(PxfServiceAddress) + strlen(LocalhostIpV4Entry) + 1);

		strcat(pxf_host_entry, PxfServiceAddress);
		strcat(pxf_host_entry, LocalhostIpV4Entry);
		resolve_hosts = curl_slist_append(NULL, pxf_host_entry);
		SetCurlOption(context, CURLOPT_RESOLVE, resolve_hosts);
		pfree(pxf_host_entry);
	}
#endif

	SetCurlOption(context, CURLOPT_URL, url);

	SetCurlOption(context, CURLOPT_VERBOSE, 0L /* FALSE */);

	/* set callback for each header received from server */
	SetCurlOption(context, CURLOPT_HEADERFUNCTION, HeaderCallback);

	/* set callback context for each header received from server */
	SetCurlOption(context, CURLOPT_HEADERDATA, context);

	/* set callback for each data block arriving from server to be written to application */
	SetCurlOption(context, CURLOPT_WRITEFUNCTION, WriteCallback);

	/* set callback context for each data block arriving from server to be written to application */
	SetCurlOption(context, CURLOPT_WRITEDATA, context);

	SetCurlOption(context, CURLOPT_IPRESOLVE, (const void *) CURL_IPRESOLVE_V4);

	/* curl will save its last error in curl_error_buffer */
	SetCurlOption(context, CURLOPT_ERRORBUFFER, context->curl_error_buffer);

	CurlHeadersSet(context, headers);

	/*
	 * SSL configuration
	 */
	if (IS_HTTPS_URI(url))
	{
		/* cert is stored PEM coded in file... */
		SetCurlOption(context, CURLOPT_SSLCERTTYPE, "PEM");

		/* set the cert for client authentication */
		if (ssl_options->client_cert_path != NULL)
		{
			elog(LOG, "attempting to load client certificate from %s", ssl_options->client_cert_path);

			if (!FileExistsAndCanRead(ssl_options->client_cert_path))
				ereport(ERROR,
					(errcode(errcode_for_file_access()),
						errmsg("could not open client certificate file \"%s\": %m", ssl_options->client_cert_path)));

			SetCurlOption(context, CURLOPT_SSLCERT, ssl_options->client_cert_path);
		}

		/* set the key passphrase */
		if (ssl_options->private_key_password != NULL)
			SetCurlOption(context, CURLOPT_KEYPASSWD, ssl_options->private_key_password);

		SetCurlOption(context, CURLOPT_SSLKEYTYPE,"PEM");

		/* set the private key (file or ID in engine) */
		if (ssl_options->private_key_path != NULL)
		{
			elog(LOG, "attempting to load private key file from %s", ssl_options->private_key_path);

			if (!FileExistsAndCanRead(ssl_options->private_key_path))
				ereport(ERROR,
					(errcode(errcode_for_file_access()),
						errmsg("could not open private key file \"%s\": %m", ssl_options->private_key_path)));

			SetCurlOption(context, CURLOPT_SSLKEY, ssl_options->private_key_path);
		}

		/* set the file with the CA certificates, for validating the server */
		if (ssl_options->trusted_ca_path != NULL)
		{
			elog(LOG, "attempting to load trusted certificate authorities file from %s", ssl_options->trusted_ca_path);

			if (!FileExistsAndCanRead(ssl_options->trusted_ca_path))
				ereport(ERROR,
					(errcode(errcode_for_file_access()),
						errmsg("could not open trusted certificate authorities file \"%s\": %m", ssl_options->trusted_ca_path)));

			SetCurlOption(context, CURLOPT_CAINFO, ssl_options->trusted_ca_path);
		}

		/* set cert verification */
		SetCurlOption(context, CURLOPT_SSL_VERIFYPEER, (const void *) (!ssl_options->disable_verification ? SSL_VERIFYPEER : SSL_NO_VERIFY));

		/* set host verification */
		SetCurlOption(context, CURLOPT_SSL_VERIFYHOST, (const void *) (!ssl_options->disable_verification ? SSL_VERIFYHOST : SSL_NO_VERIFY));

		/* set protocol */
		SetCurlOption(context, CURLOPT_SSLVERSION, (const void *) &ssl_options->version);

		/* disable session ID cache */
		SetCurlOption(context, CURLOPT_SSL_SESSIONID_CACHE, 0);

		/* set debug */
		if (CURLE_OK != (curl_error = curl_easy_setopt(context->curl_handle, CURLOPT_VERBOSE, (ssl_options->verbose ? 1L : 0L))) &&
			ssl_options->verbose)
		{
			elog(INFO, "internal error: curl_easy_setopt CURLOPT_VERBOSE error (%d - %s)",
				 curl_error, curl_easy_strerror(curl_error));
		}
	}

	return (PXF_CURL_HANDLE) context;
}

PXF_CURL_HANDLE
PxfCurlInitUpload(const char *url, PXF_CURL_HEADERS headers, PxfSSLOptions *ssl_options)
{
	curl_context *context = PxfCurlInit(url, headers, ssl_options);

	context->upload = true;

	SetCurlOption(context, CURLOPT_POST, (const void *) TRUE);
	SetCurlOption(context, CURLOPT_READFUNCTION, ReadCallback);
	SetCurlOption(context, CURLOPT_READDATA, context);
	PxfCurlHeadersAppend(headers, "Content-Type", "application/octet-stream");
	PxfCurlHeadersAppend(headers, "Transfer-Encoding", "chunked");
	PxfCurlHeadersAppend(headers, "Expect", "100-continue");

	PxfPrintHttpHeaders(headers);
	SetupMultiHandle(context);
	return (PXF_CURL_HANDLE) context;
}

PXF_CURL_HANDLE
PxfCurlInitDownload(const char *url, PXF_CURL_HEADERS headers, PxfSSLOptions *ssl_options)
{
	curl_context *context = PxfCurlInit(url, headers, ssl_options);

	context->upload = false;

	PxfPrintHttpHeaders(headers);
	SetupMultiHandle(context);
	return (PXF_CURL_HANDLE) context;
}

void
PxfCurlDownloadRestart(PXF_CURL_HANDLE handle, const char *url, PXF_CURL_HEADERS headers)
{
	curl_context *context = (curl_context *) handle;

	Assert(!context->upload);

	/* halt current transfer */
	MultiRemoveHandle(context);

	/* set a new url */
	SetCurlOption(context, CURLOPT_URL, url);

	/* set headers again */
	if (headers)
		CurlHeadersSet(context, headers);

	/* restart */
	SetupMultiHandle(context);
}

/*
 * upload
 */
size_t
PxfCurlWrite(PXF_CURL_HANDLE handle, const char *buf, size_t bufsize)
{
	curl_context *context = (curl_context *) handle;
	curl_buffer *context_buffer = context->upload_buffer;

	Assert(context->upload);

	if (!InternalBufferLargeEnough(context_buffer, bufsize))
	{
		FlushInternalBuffer(context);
		if (!InternalBufferLargeEnough(context_buffer, bufsize))
			EnlargeInternalBuffer(context_buffer, bufsize);
	}

	memcpy(context_buffer->ptr + context_buffer->top, buf, bufsize);
	context_buffer->top += bufsize;

	return bufsize;
}

/*
 * check that connection is ok, read a few bytes and check response.
 */
void
PxfCurlReadCheckConnectivity(PXF_CURL_HANDLE handle)
{
	curl_context *context = (curl_context *) handle;

	Assert(!context->upload);

	FillInternalBuffer(context, 1);
	CheckResponse(context);
}

/*
 * download
 */
size_t
PxfCurlRead(PXF_CURL_HANDLE handle, char *buf, size_t max_size)
{
	int			n = 0;
	curl_context *context = (curl_context *) handle;
	curl_buffer *context_buffer = context->download_buffer;

	Assert(!context->upload);

	FillInternalBuffer(context, max_size);

	n = context_buffer->top - context_buffer->bot;

	/*------
	 * TODO: this means we are done. Should we do something with it?
	 * if (n == 0 && !context->curl_still_running)
	 * context->eof = true;
	 *------
	 */

	if (n > max_size)
		n = max_size;

	memcpy(buf, context_buffer->ptr + context_buffer->bot, n);
	context_buffer->bot += n;

	return n;
}

void
PxfCurlCleanup(PXF_CURL_HANDLE handle, bool after_error)
{
	curl_context *context = (curl_context *) handle;

	if (!context)
		return;

	/* don't try to read/write data after an error */
	if (!after_error)
	{
		if (context->upload)
			FinishUpload(context);
		else
			PxfCurlReadCheckConnectivity(handle);
	}

	CleanupCurlHandle(context);
	CleanupInternalBuffer(context->download_buffer);
	CleanupInternalBuffer(context->upload_buffer);
	CurlCleanupContext(context);
}

static curl_context *
CurlNewContext()
{
	curl_context *context = palloc0(sizeof(curl_context));

	context->download_buffer = palloc0(sizeof(curl_buffer));
	context->upload_buffer = palloc0(sizeof(curl_buffer));
	return context;
}

static void
ClearErrorBuffer(curl_context *context)
{
	if (!context)
		return;
	context->curl_error_buffer[0] = 0;
}

static void
CreateCurlHandle(curl_context *context)
{
	context->curl_handle = curl_easy_init();
	if (!context->curl_handle)
		elog(ERROR, "internal error: curl_easy_init failed");
}

static void
SetCurlOption(curl_context *context, CURLoption option, const void *data)
{
	int		curl_error;

	if (CURLE_OK != (curl_error = curl_easy_setopt(context->curl_handle, option, data)))
		elog(ERROR, "internal error: curl_easy_setopt %d error (%d - %s)",
			 option, curl_error, curl_easy_strerror(curl_error));
}

/*
 * Called by libcurl perform during an upload.
 * Copies data from internal buffer to libcurl's buffer.
 * Once zero is returned, libcurl knows upload is over
 */
static size_t
ReadCallback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	curl_context *context = (curl_context *) userdata;
	curl_buffer *context_buffer = context->upload_buffer;

	int			written = Min(size * nmemb, context_buffer->top - context_buffer->bot);

	memcpy(ptr, context_buffer->ptr + context_buffer->bot, written);
	context_buffer->bot += written;

	return written;
}

/*
 * Setups the libcurl multi API
 */
static void
SetupMultiHandle(curl_context *context)
{
	int		curl_error;

	/* Create multi handle on first use */
	if (!context->multi_handle)
		if (!(context->multi_handle = curl_multi_init()))
			elog(ERROR, "internal error: curl_multi_init failed");

	/* add the easy handle to the multi handle */
	/* don't blame me, blame libcurl */
	if (CURLM_OK != (curl_error = curl_multi_add_handle(context->multi_handle, context->curl_handle)))
		if (CURLM_CALL_MULTI_PERFORM != curl_error)
			elog(ERROR, "internal error: curl_multi_add_handle failed (%d - %s)",
				 curl_error, curl_easy_strerror(curl_error));

	MultiPerform(context);
}

/*
 * Does the real work. Causes libcurl to do
 * as little work as possible and return.
 * During this functions execution,
 * callbacks are called.
 */
static void
MultiPerform(curl_context *context)
{
	int			curl_error;

	while (CURLM_CALL_MULTI_PERFORM ==
		   (curl_error = curl_multi_perform(context->multi_handle, &context->curl_still_running)));

	if (curl_error != CURLM_OK)
		elog(ERROR, "internal error: curl_multi_perform failed (%d - %s)",
			 curl_error, curl_easy_strerror(curl_error));
}

static bool
InternalBufferLargeEnough(curl_buffer *buffer, size_t required)
{
	return ((buffer->top + required) <= buffer->max);
}

static void
FlushInternalBuffer(curl_context *context)
{
	curl_buffer *context_buffer = context->upload_buffer;

	if (context_buffer->top == 0)
		return;

	while ((context->curl_still_running != 0) &&
		   ((context_buffer->top - context_buffer->bot) > 0))
	{
		/*
		 * Allow canceling a query while waiting for input from remote service
		 */
		CHECK_FOR_INTERRUPTS();

		MultiPerform(context);
	}

	if ((context->curl_still_running == 0) &&
		((context_buffer->top - context_buffer->bot) > 0))
		elog(ERROR, "failed sending to remote component %s", GetDestAddress(context->curl_handle));

	CheckResponse(context);

	context_buffer->top = 0;
	context_buffer->bot = 0;
}

/*
 * Returns the remote ip and port of the curl response.
 * If it's not available, returns an empty string.
 * The returned value should be free'd.
 */
static char *
GetDestAddress(CURL * curl_handle)
{
	char	   *dest_url = NULL;

	/* add dest url, if any, and curl was nice to tell us */
	if (CURLE_OK == curl_easy_getinfo(curl_handle, CURLINFO_PRIMARY_IP, &dest_url) && dest_url)
	{
		/* TODO: do not hardcode the port here */
		return psprintf("'%s:%d'", dest_url, 5888);
	}
	return dest_url;
}

static void
EnlargeInternalBuffer(curl_buffer *buffer, size_t required)
{
	buffer->max = (int) required + 1024;
	buffer->ptr = repalloc(buffer->ptr, buffer->max);
}

/*
 * Let libcurl finish the upload by
 * calling perform repeatedly
 */
static void
FinishUpload(curl_context *context)
{
	if (!context->multi_handle)
		return;

	FlushInternalBuffer(context);

	/*
	 * allow ReadCallback to say 'all done' by returning a zero thus ending
	 * the connection
	 */
	while (context->curl_still_running != 0)
		MultiPerform(context);

	CheckResponse(context);
}

static void
CleanupCurlHandle(curl_context *context)
{
	if (!context->curl_handle)
		return;
	if (context->multi_handle)
		MultiRemoveHandle(context);
	curl_easy_cleanup(context->curl_handle);
	context->curl_handle = NULL;
	curl_multi_cleanup(context->multi_handle);
	context->multi_handle = NULL;
}

static void
MultiRemoveHandle(curl_context *context)
{
	int			curl_error;

	Assert(context->curl_handle && context->multi_handle);

	if (CURLM_OK !=
		(curl_error = curl_multi_remove_handle(context->multi_handle, context->curl_handle)))
		elog(ERROR, "internal error: curl_multi_remove_handle failed (%d - %s)",
			 curl_error, curl_easy_strerror(curl_error));
}

static void
CleanupInternalBuffer(curl_buffer *buffer)
{
	if ((buffer) && (buffer->ptr))
	{
		pfree(buffer->ptr);
		buffer->ptr = NULL;
		buffer->bot = 0;
		buffer->top = 0;
		buffer->max = 0;
	}
}

static void
CurlCleanupContext(curl_context *context)
{
	if (context)
	{
		if (context->download_buffer)
			pfree(context->download_buffer);
		if (context->upload_buffer)
			pfree(context->upload_buffer);

		pfree(context);
	}
}

/*
 * WriteCallback
 *
 * Called by libcurl perform during a download.
 * Stores data from libcurl's buffer into the internal buffer.
 * If internal buffer is not large enough, increases it.
 *
 * we return the number of bytes written to the application buffer
 */
static size_t
WriteCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
	curl_context *context = (curl_context *) userp;
	curl_buffer *context_buffer = context->download_buffer;
	const int	nbytes = size * nitems;

	if (!InternalBufferLargeEnough(context_buffer, nbytes))
	{
		CompactInternalBuffer(context_buffer);
		if (!InternalBufferLargeEnough(context_buffer, nbytes))
			ReallocInternalBuffer(context_buffer, nbytes);
	}

	/* enough space. copy buffer into curl->buf */
	memcpy(context_buffer->ptr + context_buffer->top, buffer, nbytes);
	context_buffer->top += nbytes;

	return nbytes;
}

/*
 * Fills internal buffer up to want bytes.
 * returns when size reached or transfer ended
 */
static void
FillInternalBuffer(curl_context *context, int want)
{
	fd_set		fdread;
	fd_set		fdwrite;
	fd_set		fdexcep;
	struct		timeval timeout;
	int		maxfd, nfds, curl_error, timeout_count = 0;
	long		curl_timeo = -1;

	/* attempt to fill buffer */
	while (context->curl_still_running &&
		   ((context->download_buffer->top - context->download_buffer->bot) < want))
	{
		FD_ZERO(&fdread);
		FD_ZERO(&fdwrite);
		FD_ZERO(&fdexcep);

		/* allow canceling a query while waiting for input from remote service */
		CHECK_FOR_INTERRUPTS();

		/* set a suitable timeout to fail on */
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		curl_multi_timeout(context->multi_handle, &curl_timeo);
		if (curl_timeo >= 0)
		{
			timeout.tv_sec = curl_timeo / 1000;
			if (timeout.tv_sec > 1)
				timeout.tv_sec = 1;
			else
				timeout.tv_usec = (curl_timeo % 1000) * 1000;
		}

		/* get file descriptors from the transfers */
		curl_error = curl_multi_fdset(context->multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
		if (CURLE_OK != curl_error)
		{
			elog(ERROR, "internal error: curl_multi_fdset failed (%d - %s)",
				 curl_error, curl_easy_strerror(curl_error));
		}

		if (maxfd == -1)
		{
			/* curl is not ready if maxfd -1 is returned */
			context->curl_still_running = 0;
			pg_usleep(100);
		}
		else if ((nfds = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout)) == -1)
		{
			if (errno == EINTR || errno == EAGAIN)
			{
				elog(DEBUG2, "select failed on curl_multi_fdset (maxfd %d) (%d - %s)", maxfd, errno, strerror(errno));
				continue;
			}
			elog(ERROR, "internal error: select failed on curl_multi_fdset (maxfd %d) (%d - %s)",
				 maxfd, errno, strerror(errno));
		}
		else if (nfds == 0)
		{
			// timeout
			timeout_count++;

			if (timeout_count % 60 == 0)
			{
				elog(LOG, "segment has not received data from PXF Server for about 1 minute, waiting for %d bytes.",
					(want - (context->download_buffer->top - context->download_buffer->bot)));
			}
		}
		else if (nfds < 0)
		{
			elog(ERROR, "select return unexpected result");
		}
		MultiPerform(context);
	}
}

static void
CurlHeadersSet(curl_context *context, PXF_CURL_HEADERS headers)
{
	churl_settings *settings = (churl_settings *) headers;

	SetCurlOption(context, CURLOPT_HTTPHEADER, settings->headers);
}

/*
 * Checks that the response finished successfully
 * with a valid response status and code.
 */
static void
CheckResponse(curl_context *context)
{
	CheckResponseCode(context);
	CheckResponseStatus(context);
}

/*
 * Checks that libcurl transfers completed successfully.
 * This is different than the response code (HTTP code) -
 * a message can have a response code 200 (OK), but end prematurely
 * and so have an error status.
 */
static void
CheckResponseStatus(curl_context *context)
{
	CURLMsg    *msg;			/* for picking up messages with the transfer
								 * status */
	int			msgs_left;		/* how many messages are left */
	long		status;

	while ((msg = curl_multi_info_read(context->multi_handle, &msgs_left)))
	{
		int			i = 0;

		/* CURLMSG_DONE is the only possible status. */
		if (msg->msg != CURLMSG_DONE)
			continue;
		if (CURLE_OK != (status = msg->data.result))
		{
			StringInfoData err;
			initStringInfo(&err);
			appendStringInfo(&err, "transfer error (%ld): %s",
							 status, curl_easy_strerror(status));

			elog(ERROR, "%s", err.data);
		}
		elog(DEBUG2, "CheckResponseStatus: msg %d done with status OK", i++);
	}
}

/*
 * Parses return code from libcurl operation and
 * reports if different than 200 and 100
 */
static void
CheckResponseCode(curl_context *context)
{
	long		response_code;
	char		*response_text = NULL;
	int		curl_error;

	if (CURLE_OK != (curl_error = curl_easy_getinfo(context->curl_handle, CURLINFO_RESPONSE_CODE, &response_code)))
	{
		elog(ERROR, "internal error: curl_easy_getinfo failed(%d - %s)",
			 curl_error, curl_easy_strerror(curl_error));
	}

	elog(DEBUG2, "http response code: %ld", response_code);
	if ((response_code == 0) && (context->curl_still_running > 0))
	{
		elog(DEBUG2, "CheckResponseCode: curl is still running, but no data was received.");
	}
	else if (response_code != 200 && response_code != 100)
	{
		StringInfoData err;
		char	   *http_error_msg,
				   *trace_msg = NULL;

		initStringInfo(&err);

		/* prepare response text if any */
		if (context->download_buffer->ptr)
		{
			context->download_buffer->ptr[context->download_buffer->top] = '\0';
			response_text = context->download_buffer->ptr + context->download_buffer->bot;
		}

		/* add remote http error code */
		appendStringInfo(&err, "PXF server error (%ld)", response_code);

		if (!HandleSpecialError(response_code, &err))
		{
			/*
			 * add detailed error message from the http response.
			 * response_text could be NULL in some cases. GetHttpErrorMsg
			 * checks for that.
			 */
			http_error_msg = GetHttpErrorMsg(response_code, response_text, context->curl_error_buffer, &trace_msg);

			appendStringInfo(&err, ": %s", http_error_msg);
		}

		if (trace_msg != NULL)
		{
			ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				errmsg("%s", err.data),
				errhint("%s", trace_msg)));
		}
		else
		{
			ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				errmsg("%s", err.data),
				errhint("Check the PXF logs located in the '$PXF_CONF/logs' directory or 'set client_min_messages=DEBUG1' for additional details")));
		}
	}

	FreeHttpResponse(context);
}

/*
 * Extracts the error message from the full HTTP response
 * We test for several conditions in the http_ret_code and the HTTP response message.
 * The first condition that matches, defines the final message string and ends the function.
 * The layout of the HTTP response message is:

 {
  "timestamp": "the server timestamp",
  "status": status code int,
  "error": "error description",
  "message": "error message",
  "trace": "the stack trace for the error",
  "path": "uri for the request"
 }

 * We try to get the message and trace.
 */
static char*
GetHttpErrorMsg(long http_ret_code, char *msg, char *curl_error_buffer, char **trace_message)
{
	char	   *res,
			   *fmessagestr = "message",
			   *ftracestr = "trace";
	Datum	    result;
	StringInfoData errMsg;
	FmgrInfo *json_object_field_text_fn;

	initStringInfo(&errMsg);
	*trace_message = NULL;

	/*
	 * 1. The server not listening on the port specified in the <create
	 * external...> statement" In this case there is no Response from the
	 * server, so we issue our own message
	 */

	if (http_ret_code == 0)
	{
		if (curl_error_buffer == NULL)
			return "There is no PXF server listening on the host and port specified in the PXF configuration";
		else
			return curl_error_buffer;
	}

	/*
	 * 2. There is a response from the server since the http_ret_code is not
	 * 0, but there is no response message. This is an abnormal situation that
	 * could be the result of a bug, libraries incompatibility or versioning
	 * issue in the Rest server or our curl client. In this case we again
	 * issue our own message.
	 */
	if (!msg || strlen(msg) == 0)
	{
		appendStringInfo(&errMsg, "HTTP status code is %ld but HTTP response string is empty", http_ret_code);
		res = pstrdup(errMsg.data);
		pfree(errMsg.data);
		return res;
	}

	/*
	 * 3. The "normal" case - There is an HTTP response and we parse the
	 * json response fields "message" and "trace"
	 */

	json_object_field_text_fn = palloc(sizeof(FmgrInfo));

	/* find the json_object_field_text function */
	fmgr_info(F_JSON_OBJECT_FIELD_TEXT, json_object_field_text_fn);

	if ((DEBUG1 >= log_min_messages) || (DEBUG1 >= client_min_messages))
	{
		/* get the "trace" field from the json error */
		result = FunctionCall2(json_object_field_text_fn,
			PointerGetDatum(cstring_to_text(msg)),
			PointerGetDatum(cstring_to_text(ftracestr)));

		if (DatumGetPointer(result) != NULL)
			*trace_message = text_to_cstring(DatumGetTextP(result));
	}

	/* get the "message" field from the json error */
	result = FunctionCall2(json_object_field_text_fn,
		PointerGetDatum(cstring_to_text(msg)),
		PointerGetDatum(cstring_to_text(fmessagestr)));

	pfree(json_object_field_text_fn);

	if (DatumGetPointer(result) != NULL)
	{
		return text_to_cstring(DatumGetTextP(result));
	}

	/*
	 * 4. This is an unexpected situation. We received an error message from
	 * the server but it does not have a "message" field. In this case we
	 * return the error message we received as-is.
	 */
	return msg;
}

static void
FreeHttpResponse(curl_context *context)
{
	if (!context->last_http_reponse)
		return;

	pfree(context->last_http_reponse);
	context->last_http_reponse = NULL;
}

/*
 * Called during a perform by libcurl on either download or an upload.
 * Stores the first line of the header for error reporting
 */
static size_t
HeaderCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
	const int	nbytes = size * nitems;
	curl_context *context = (curl_context *) userp;

	if (context->last_http_reponse)
		return nbytes;

	char	   *p = palloc(nbytes + 1);

	memcpy(p, buffer, nbytes);
	p[nbytes] = 0;
	context->last_http_reponse = p;

	return nbytes;
}

static void
CompactInternalBuffer(curl_buffer *buffer)
{
	int			n;

	/* no compaction required */
	if (buffer->bot == 0)
		return;

	n = buffer->top - buffer->bot;
	memmove(buffer->ptr, buffer->ptr + buffer->bot, n);
	buffer->bot = 0;
	buffer->top = n;
}

static void
ReallocInternalBuffer(curl_buffer *buffer, size_t required)
{
	int			n;

	n = buffer->top - buffer->bot + required + 1024;
	if (buffer->ptr == NULL)
		buffer->ptr = palloc(n);
	else
		/* repalloc does not support NULL ptr */
		buffer->ptr = repalloc(buffer->ptr, n);

	buffer->max = n;

	Assert(buffer->top + required < buffer->max);
}

static bool
HandleSpecialError(long response, StringInfo err)
{
	if (response == 404)
	{
		appendStringInfo(err, ": PXF service could not be reached. PXF is not running in the tomcat container");
		return true;
	}
	return false;
}

static bool
FileExistsAndCanRead(char *filename)
{
	FILE* file;
	if ((file = fopen(filename, "r")) > 0)
	{
		fclose(file);
		return 1;
	}
	return 0;
}
