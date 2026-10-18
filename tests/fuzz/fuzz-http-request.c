/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The receiver's HTTP request parser (uxplay/lib/http_request.c), fed in
 * pieces the way a socket delivers them: the first byte sets the size of
 * each piece. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "http_request.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;
    const size_t piece = 1 + data[0];
    http_body_budget_t budget = {0, 1024 * 1024};
    http_request_t *request = http_request_init();
    if (!request) return 0;
    http_request_set_body_policy(request, &budget, NULL, NULL);
    for (size_t at = 1; at < size; at += piece) {
        const size_t length = size - at < piece ? size - at : piece;
        if (http_request_add_data(request, (const char *)data + at, (int)length) < 0 ||
            http_request_has_error(request)) {
            break;
        }
        if (http_request_is_complete(request)) {
            int datalen = 0;
            http_request_get_method(request);
            http_request_get_url(request);
            http_request_get_header(request, "Content-Length");
            http_request_get_header(request, "X-Apple-Session-ID");
            http_request_get_data(request, &datalen);
            char *headers = NULL;
            if (http_request_get_header_string(request, &headers) >= 0) {
                free(headers);
            }
            break;
        }
    }
    http_request_destroy(request);
    return 0;
}
