#include "ftl.h"
#include "ftl_private.h"
#include <curl/curl.h>
#include <jansson.h>

static int _ingest_lookup_ip(const char *ingest_location, char ***ingest_ip);
static int _ingest_compute_score(ftl_ingest_t *ingest);
static int _ping_server(const char *ip, int port);

typedef struct {
	ftl_ingest_t *ingest;
	ftl_stream_configuration_private_t *ftl;
}_tmp_ingest_thread_data_t;

static size_t _curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct MemoryStruct *mem = (struct MemoryStruct *)userp;

	mem->memory = realloc(mem->memory, mem->size + realsize + 1);
	if (mem->memory == NULL) {
		/* out of memory! */
		printf("not enough memory (realloc returned NULL)\n");
		return 0;
	}

	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;

	return realsize;
}

int _ingest_get_hosts(ftl_stream_configuration_private_t *ftl) {
	CURL *curl_handle;
	CURLcode res;
	struct MemoryStruct chunk;
	char *query_result = NULL;
	size_t i = 0;
	int total_ingest_cnt = 0;
	json_error_t error;
	json_t *ingests = NULL, *ingest_item = NULL;

	curl_handle = curl_easy_init();

	chunk.memory = malloc(1);  /* will be grown as needed by realloc */
	chunk.size = 0;    /* no data at this point */

	curl_easy_setopt(curl_handle, CURLOPT_URL, INGEST_LIST_URI);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, TRUE);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, _curl_write_callback);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ftlsdk/1.0");

#if LIBCURL_VERSION_NUM >= 0x072400
	// A lot of servers don't yet support ALPN
	curl_easy_setopt(curl_handle, CURLOPT_SSL_ENABLE_ALPN, 0);
#endif

	res = curl_easy_perform(curl_handle);

	if (res != CURLE_OK) {
		printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		goto cleanup;
	}

	if ((ingests = json_loadb(chunk.memory, chunk.size, 0, &error)) == NULL) {
		goto cleanup;
	}
	
	size_t size = json_array_size(ingests);

	for (i = 0; i < size; i++) {
		const char *name, *host;
		ingest_item = json_array_get(ingests, i);
		json_unpack(ingest_item, "{s:s, s:s}", "name", &name, "host", &host);

		int total_ips;
		int ii;
		char **ips;
		if ((total_ips = _ingest_lookup_ip(host, &ips)) <= 0) {
			continue;
		}

		for (ii = 0; ii < total_ips; ii++) {

			ftl_ingest_t *ingest_elmt;

			if ((ingest_elmt = malloc(sizeof(ftl_ingest_t))) == NULL) {
				goto cleanup;
			}

			strcpy_s(ingest_elmt->name, sizeof(ingest_elmt->name), name);
			strcpy_s(ingest_elmt->host, sizeof(ingest_elmt->host), host);
			strcpy_s(ingest_elmt->ip, sizeof(ingest_elmt->ip), ips[ii]);
			ingest_elmt->rtt = 500;
			ingest_elmt->load_score = 100;
			free(ips[ii]);

			ingest_elmt->next = NULL;

			if (ftl->ingest_list == NULL) {
				ftl->ingest_list = ingest_elmt;
			}
			else {
				ftl_ingest_t *tail = ftl->ingest_list;
				while (tail->next != NULL) {
					tail = tail->next;
				}

				tail->next = ingest_elmt;
			}

			total_ingest_cnt++;
		}

		free(ips);
	}

cleanup:
	free(chunk.memory);
	curl_easy_cleanup(curl_handle);
	if (ingests != NULL) {
		json_decref(ingests);
	}

	ftl->ingest_count = total_ingest_cnt;

	return total_ingest_cnt;
}

OS_THREAD_ROUTINE _ingest_get_load(void *data) {

	_tmp_ingest_thread_data_t *thread_data = (_tmp_ingest_thread_data_t *)data;
	ftl_stream_configuration_private_t *ftl = thread_data->ftl;
	ftl_ingest_t *ingest = thread_data->ingest;
	int ret = 0;
	CURL *curl_handle;
	CURLcode res;
	struct MemoryStruct chunk;
	json_error_t error;
	json_t *load = NULL;
	char score_url[1024];
	char host_and_ip[100];
	struct timeval start, stop, delta;
	struct curl_slist *host = NULL;
	int ping;

    ingest->rtt = 500;
    ingest->load_score = 100;

	if ((ping = _ping_server(ingest->ip, INGEST_PING_PORT)) >= 0) {
		ingest->rtt = ping;
	}
	
	curl_handle = curl_easy_init();

	chunk.memory = malloc(1);  /* will be grown as needed by realloc */
	chunk.size = 0;    /* no data at this point */

	sprintf_s(score_url, sizeof(score_url), "https://%s:%d?id=%d&key=%s", ingest->host, INGEST_LOAD_PORT, ftl->channel_id, ftl->key);
	sprintf_s(host_and_ip, sizeof(host_and_ip), "%s:%d:%s", ingest->host, INGEST_LOAD_PORT, ingest->ip);

	host = curl_slist_append(NULL, host_and_ip);

	curl_easy_setopt(curl_handle, CURLOPT_URL, score_url);
	curl_easy_setopt(curl_handle, CURLOPT_RESOLVE, host);
	curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1); //need this for linux otherwise subsecond timeouts dont work
	curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, 1000);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, _curl_write_callback);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ftlsdk/1.0");
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, TRUE);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2L);

#if LIBCURL_VERSION_NUM >= 0x072400
	// A lot of servers don't yet support ALPN
	curl_easy_setopt(curl_handle, CURLOPT_SSL_ENABLE_ALPN, 0);
#endif

	if( (res = curl_easy_perform(curl_handle)) != CURLE_OK){
		ret = -1;
		printf("Failed to query %s: %s\n", ingest->name, curl_easy_strerror(res));
		goto cleanup;		
	}

	if ((load = json_loadb(chunk.memory, chunk.size, 0, &error)) == NULL) {
		ret = -3;
		goto cleanup;
	}

	double load_score;
	if (json_unpack(load, "{s:f}", "LoadScore", &load_score) < 0) {
		ret = -4;
		goto cleanup;
	}

	ingest->load_score = (float)load_score;

cleanup:
	free(chunk.memory);
	curl_easy_cleanup(curl_handle);

	return (OS_THREAD_TYPE)ret;
}

char * ingest_get_ip(ftl_stream_configuration_private_t *ftl, char *host) {
	if (ftl->ingest_list == NULL) {
		if (_ingest_get_hosts(ftl) <= 0) {
			return NULL;
		}
	}

	ftl_ingest_t * elmt = ftl->ingest_list;

	while (elmt != NULL) {
		if (strcmp(host, elmt->host) == 0) {
			/*just find first in list with matching host, these are on rr dns so first items will be different each time*/
			return elmt->ip;
		}

		elmt = elmt->next;
	}

	return NULL;
}

char * ingest_find_best(ftl_stream_configuration_private_t *ftl) {

	OS_THREAD_HANDLE *handle;
	_tmp_ingest_thread_data_t *data;
	int i;
	ftl_ingest_t *elmt, *best = NULL;
	struct timeval start, stop, delta;
	float best_ingest_score = 100, ingest_score;

	if (ftl->ingest_list == NULL) {
		if (_ingest_get_hosts(ftl) <= 0) {
			return NULL;
		}
	}

	if ((handle = (OS_THREAD_HANDLE *)malloc(sizeof(OS_THREAD_HANDLE) * ftl->ingest_count)) == NULL) {
		return NULL;
	}

	if ((data = (_tmp_ingest_thread_data_t *)malloc(sizeof(_tmp_ingest_thread_data_t) * ftl->ingest_count)) == NULL) {
		return NULL;
	}

	gettimeofday(&start, NULL);

	/*query all the ingests about cpu and rtt*/
	elmt = ftl->ingest_list;
	for (i = 0; i < ftl->ingest_count && elmt != NULL; i++) {
		handle[i] = 0;
		data[i].ingest = elmt;
		data[i].ftl = ftl;
		os_create_thread(&handle[i], NULL, _ingest_get_load, &data[i]);
		sleep_ms(5);
		elmt = elmt->next;
	}

	/*wait for all the ingests to complete*/
	elmt = ftl->ingest_list;
	for (i = 0; i < ftl->ingest_count && elmt != NULL; i++) {

		if (handle[i] != 0) {
			os_wait_thread(handle[i]);
		}

		ingest_score = (float)_ingest_compute_score(elmt);

		if (ingest_score < best_ingest_score ) {
			best_ingest_score = ingest_score;
			best = elmt;
		}

		elmt = elmt->next;
	}

	gettimeofday(&stop, NULL);
	timeval_subtract(&delta, &stop, &start);
	int ms = (int)timeval_to_ms(&delta);

	printf("Took %d ms to query all ingests\n", ms);

	elmt = ftl->ingest_list;
	for (i = 0; i < ftl->ingest_count && elmt != NULL; i++) {
		if (handle[i] != 0) {
			os_destroy_thread(handle[i]);
		}

		elmt = elmt->next;
	}

	free(handle);
	free(data);

	if (best){
		FTL_LOG(ftl, FTL_LOG_INFO, "%s at ip %s had the shortest RTT of %d ms with a server score of %2.1f\n", best->name, best->ip, best->rtt, best->load_score * 100.f);
		return best->ip;
	}


	return NULL;
}

static int _ingest_compute_score(ftl_ingest_t *ingest) {

	float load_score, rtt_score;
	float load_factor;

	load_score = ingest->load_score * 100.f;

	if (load_score > 100) {
		load_score = 100;
	}

	//the highest possible rtt will be 500ms
	rtt_score = (float)ingest->rtt / 500.f * 100.f;

	if (rtt_score > 100) {
		rtt_score = 100;
	}

	//unless the score is above 70, it use the rtt exclusively, when above 70 start weighting load score much higher than rtt
	if (load_score <= 70) {
		load_factor = 0;
	}
	else if (load_score < 80) {
		load_factor = 0.2;
	}
	else if (load_score < 85) {
		load_factor = 0.4;
	}
	else if (load_score < 90) {
		load_factor = 0.6;
	}
	else if (load_score < 95) {
		load_factor = 0.8;
	}
	else if (load_score >= 95) {
		load_factor = 1;
	}

	load_score = load_score * load_factor;
	rtt_score = rtt_score * (1 - load_factor);

	return (int)load_score + rtt_score;
}

static int _ingest_lookup_ip(const char *ingest_location, char ***ingest_ip) {
	struct hostent *remoteHost;
	struct in_addr addr;
	int ips_found = 0;
	BOOL success = FALSE;
	ingest_ip[0] = '\0';

	if (*ingest_ip != NULL) {
		return -1;
	}

	remoteHost = gethostbyname(ingest_location);

	if (remoteHost) {
		if (remoteHost->h_addrtype == AF_INET)
		{
			int total_ips = 0;
			while (remoteHost->h_addr_list[total_ips++] != 0);

			if ((*ingest_ip = malloc(sizeof(char*) * total_ips)) == NULL) {
				return 0;
			}

			while (remoteHost->h_addr_list[ips_found] != 0) {
				addr.s_addr = *(u_long *)remoteHost->h_addr_list[ips_found];

				if (((*ingest_ip)[ips_found] = malloc(IPV4_ADDR_ASCII_LEN)) == NULL) {
					return 0;
				}

				strcpy_s((*ingest_ip)[ips_found], IPV4_ADDR_ASCII_LEN, inet_ntoa(addr));

				ips_found++;
			}
		}
	}

	return ips_found;
}

static int _ping_server(const char *ip, int port) {

	SOCKET sock;
	struct hostent *server = NULL;
	struct sockaddr_in server_addr;
	uint8_t dummy[4];
	struct timeval start, stop, delta;
	int retval = -1;

	do {
		if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET)
		{
			break;
		}

		if ((server = gethostbyname(ip)) == NULL) {
			break;
		}

		//Prepare the sockaddr_in structure
		server_addr.sin_family = AF_INET;
		memcpy((char *)&server_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
		server_addr.sin_port = htons(port);

		set_socket_recv_timeout(sock, 500);

		gettimeofday(&start, NULL);

		if (sendto(sock, dummy, sizeof(dummy), 0, (struct sockaddr*) &server_addr, sizeof(struct sockaddr_in)) == SOCKET_ERROR) {
			break;
		}

		if (recv(sock, dummy, sizeof(dummy), 0) < 0) {
			break;
		}

		gettimeofday(&stop, NULL);
		timeval_subtract(&delta, &stop, &start);
		retval = (int)timeval_to_ms(&delta);
	} while (0);

	shutdown_socket(sock, SD_BOTH);
	close_socket(sock);

	return retval;
}