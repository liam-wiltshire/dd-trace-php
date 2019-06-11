#include "macros.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "compatibility.h"
#include "coms.h"
#include "coms_curl.h"
#include "env_config.h"
#include "vendor_stdatomic.h"

#define HOST_FORMAT_STR "http://%s:%u/v0.4/traces"
#define DEFAULT_FLUSH_INTERVAL (5000)
#define DEFAULT_FLUSH_AFTER_N_REQUESTS (10)
#define DEFAULT_AGENT_CONNECT_TIMEOUT (100)
#define DEFAULT_AGENT_TIMEOUT (500)

struct _writer_loop_data_t {
    pthread_t thread;
    pthread_mutex_t mutex, finished_flush_mutex;
    pthread_cond_t condition, finished_flush_condition;

    BOOL_T running;  // needs to be guarded by mutex
    _Atomic(BOOL_T) shutdown, send;
    _Atomic(uint32_t) request_counter, flush_counter, requests_since_last_flush;
};

inline static uint32_t get_flush_interval() {
    return ddtrace_get_uint32_config("DD_TRACE_AGENT_FLUSH_INTERVAL", DEFAULT_FLUSH_INTERVAL);
}

inline static uint32_t get_flush_after_n_requests() {
    return ddtrace_get_uint32_config("DD_TRACE_AGENT_FLUSH_AFTER_N_REQUESTS", DEFAULT_FLUSH_AFTER_N_REQUESTS);
}

inline static void curl_set_timeout(CURL *curl) {
    uint32_t agent_timeout = ddtrace_get_uint32_config("DD_TRACE_AGENT_TIMEOUT", DEFAULT_AGENT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, agent_timeout);
}

inline static void curl_set_connect_timeout(CURL *curl) {
    uint32_t agent_connect_timeout =
        ddtrace_get_uint32_config("DD_TRACE_AGENT_CONNECT_TIMEOUT", DEFAULT_AGENT_CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, agent_connect_timeout);
}

inline static void curl_set_hostname(CURL *curl) {
    char *hostname = ddtrace_get_c_string_config("DD_AGENT_HOST");
    int64_t port = ddtrace_get_int_config("DD_TRACE_AGENT_PORT", 8126);
    if (port <= 0 || port > 65535) {
        port = 8126;
    }

    if (hostname) {
        size_t agent_url_len =
            strlen(hostname) + sizeof(HOST_FORMAT_STR) + 10;  // port digit allocation + some headroom
        char *agent_url = malloc(agent_url_len);
        snprintf(agent_url, agent_url_len, HOST_FORMAT_STR, hostname, (uint32_t)port);

        curl_easy_setopt(curl, CURLOPT_URL, agent_url);
        ddtrace_env_free(hostname);
        free(agent_url);
    } else {
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8126/v0.4/traces");
    }
}

inline static struct timespec deadline_in_ms(uint32_t ms) {
    struct timespec deadline;
    struct timeval now;

    gettimeofday(&now, NULL);
    uint32_t sec = ms / 1000UL;
    uint32_t msec = ms % 1000UL;
    deadline.tv_sec = now.tv_sec + sec;
    deadline.tv_nsec = ((now.tv_usec + 1000UL * msec) * 1000UL);

    // carry over full seconds from nsec
    deadline.tv_sec += deadline.tv_nsec / (1000 * 1000 * 1000);
    deadline.tv_nsec %= (1000 * 1000 * 1000);

    return deadline;
}

inline static BOOL_T curl_debug() { return ddtrace_get_bool_config("DD_TRACE_DEBUG_CURL_OUTPUT", FALSE); }

static size_t dummy_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    UNUSED(userdata);
    size_t data_length = size * nmemb;
    if (curl_debug()) {
        printf("%s", ptr);
    }
    return data_length;
}

#ifndef __clang__
// disable checks since older GCC is throwing false errors
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static struct _writer_loop_data_t global_writer = {
    .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER, .shutdown = {0}, .send = {0}};
#pragma GCC diagnostic pop
#else  //__clang__
static struct _writer_loop_data_t global_writer = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                                                   .condition = PTHREAD_COND_INITIALIZER};
#endif

inline static struct _writer_loop_data_t *get_writer() { return &global_writer; }

inline static void curl_send_stack(ddtrace_coms_stack_t *stack) {
    CURL *curl = curl_easy_init();
    if (curl) {
        CURLcode res;
        curl_set_hostname(curl);
        curl_set_timeout(curl);
        curl_set_connect_timeout(curl);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
        headers = curl_slist_append(headers, "Content-Type: application/msgpack");

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE, 10);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);

        void *read_data = ddtrace_init_read_userdata(stack);

        curl_easy_setopt(curl, CURLOPT_READDATA, read_data);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ddtrace_coms_read_callback);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dummy_write_callback);

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            if (curl_debug()) {
                printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
                fflush(stdout);
            }
        } else {
            if (curl_debug()) {
                double uploaded;
                curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD, &uploaded);
                printf("UPLOADED %.0f bytes\n", uploaded);
                fflush(stdout);
            }
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);

        ddtrace_deinit_read_userdata(read_data);
    }
}

static void *writer_loop(void *_) {
    UNUSED(_);
    struct _writer_loop_data_t *writer = get_writer();
    pthread_mutex_lock(&writer->mutex);
    writer->running = TRUE;
    pthread_mutex_unlock(&writer->mutex);

    do {
        if (!atomic_load(&writer->shutdown)) {
            struct timespec deadline = deadline_in_ms(get_flush_interval());

            pthread_mutex_lock(&writer->mutex);
            pthread_cond_timedwait(&writer->condition, &writer->mutex, &deadline);
            pthread_mutex_unlock(&writer->mutex);
        }

        ddtrace_coms_rotate_stack();
        atomic_store(&writer->requests_since_last_flush, 0);

        ddtrace_coms_stack_t *stack;
        ddtrace_coms_rotate_stack();
        while ((stack = ddtrace_coms_attempt_acquire_stack())) {
            if (atomic_load(&writer->send)) {
                curl_send_stack(stack);
            }
            ddtrace_coms_free_stack(stack);
        }
        atomic_fetch_add(&writer->flush_counter, 1);
        pthread_cond_signal(&writer->finished_flush_condition);
        fflush(stdout);

    } while (!atomic_load(&writer->shutdown));

    pthread_exit(NULL);
    return NULL;
}

BOOL_T ddtrace_coms_set_writer_send_on_flush(BOOL_T send) {
    struct _writer_loop_data_t *writer = get_writer();
    BOOL_T previous_value = atomic_load(&writer->send);
    atomic_store(&writer->send, send);

    return previous_value;
}

BOOL_T ddtrace_coms_init_and_start_writer() {
    struct _writer_loop_data_t *writer = get_writer();
    atomic_store(&writer->send, TRUE);
    atomic_store(&writer->shutdown, FALSE);
    if (pthread_create(&writer->thread, NULL, &writer_loop, NULL) == 0) {
        return TRUE;
    }

    return FALSE;
}

BOOL_T ddtrace_coms_trigger_writer_flush() {
    struct _writer_loop_data_t *writer = get_writer();

    pthread_mutex_lock(&writer->mutex);
    pthread_cond_signal(&writer->condition);
    pthread_mutex_unlock(&writer->mutex);

    return TRUE;
}

BOOL_T ddtrace_coms_on_request_finished() {
    struct _writer_loop_data_t *writer = get_writer();

    atomic_fetch_add(&writer->request_counter, 1);
    uint32_t requests_since_last_flush = atomic_fetch_add(&writer->requests_since_last_flush, 1);

    // simple heuristic to flush every n request to reduce the number of memory held
    if (requests_since_last_flush > get_flush_after_n_requests()) {
        ddtrace_coms_trigger_writer_flush();
    }

    return TRUE;
}

BOOL_T ddtrace_coms_shutdown_writer(BOOL_T immediate) {
    struct _writer_loop_data_t *writer = get_writer();
    atomic_store(&writer->shutdown, TRUE);
    if (immediate) {
        ddtrace_coms_trigger_writer_flush();
    }

    if (pthread_mutex_trylock(&writer->mutex) == 0) {
        BOOL_T should_join = writer->running;
        writer->running = FALSE;
        pthread_mutex_unlock(&writer->mutex);
        if (should_join) {
            void *ptr;
            pthread_join(writer->thread, &ptr);
        }
    }

    return TRUE;
}

BOOL_T ddtrace_coms_syncronous_flush() {
    struct _writer_loop_data_t *writer = get_writer();
    uint32_t previous_flush_counter = atomic_load(&writer->flush_counter);

    ddtrace_coms_trigger_writer_flush();

    while (previous_flush_counter == atomic_load(&writer->flush_counter)) {
        pthread_mutex_lock(&writer->finished_flush_mutex);
        struct timespec deadline = deadline_in_ms(100);
        pthread_cond_timedwait(&writer->finished_flush_condition, &writer->finished_flush_mutex, &deadline);
        pthread_mutex_unlock(&writer->finished_flush_mutex);
    }
}
