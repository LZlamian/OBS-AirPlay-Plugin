/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows test stand-in for Bonjour's dnssd.dll: UxPlay's dnssd_init loads
   it even when nothing is registered (the protocol smoke only needs the
   context for GET /info). Built next to the test executables, which
   LoadLibrary searches first. TXT records work; registering fails as it
   does with the Bonjour service stopped. Never packaged. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STUB_EXPORT __declspec(dllexport)

typedef struct {
    uint8_t *bytes;
    uint16_t length;
} txt_t;

/* Same size as dns_sd.h's TXTRecordRef (16 bytes). */
typedef union {
    char private_data[16];
    txt_t *txt;
} txt_ref_t;

STUB_EXPORT int32_t DNSServiceRegister(void *ref, uint32_t flags, uint32_t interface_index,
                                       const char *name, const char *type, const char *domain,
                                       const char *host, uint16_t port, uint16_t txt_length,
                                       const void *txt, void *callback, void *context)
{
    (void) ref; (void) flags; (void) interface_index; (void) name; (void) type; (void) domain;
    (void) host; (void) port; (void) txt_length; (void) txt; (void) callback; (void) context;
    return -65563; /* kDNSServiceErr_ServiceNotRunning */
}

STUB_EXPORT void DNSServiceRefDeallocate(void *ref)
{
    (void) ref;
}

STUB_EXPORT void TXTRecordCreate(txt_ref_t *record, uint16_t buffer_length, void *buffer)
{
    (void) buffer_length; (void) buffer;
    record->txt = calloc(1, sizeof(txt_t));
}

STUB_EXPORT int32_t TXTRecordSetValue(txt_ref_t *record, const char *key, uint8_t value_size,
                                      const void *value)
{
    txt_t *txt = record->txt;
    const size_t key_length = strlen(key);
    const size_t item = key_length + (value ? 1U + value_size : 0U);
    if (!txt || item > 255 || txt->length + 1U + item > UINT16_MAX) return -65540;
    uint8_t *bytes = realloc(txt->bytes, txt->length + 1U + item);
    if (!bytes) return -65539;
    txt->bytes = bytes;
    bytes += txt->length;
    *bytes++ = (uint8_t) item;
    memcpy(bytes, key, key_length);
    if (value) {
        bytes[key_length] = '=';
        memcpy(bytes + key_length + 1, value, value_size);
    }
    txt->length = (uint16_t) (txt->length + 1U + item);
    return 0;
}

STUB_EXPORT uint16_t TXTRecordGetLength(const txt_ref_t *record)
{
    return record->txt ? record->txt->length : 0;
}

STUB_EXPORT const void *TXTRecordGetBytesPtr(const txt_ref_t *record)
{
    return record->txt ? record->txt->bytes : NULL;
}

STUB_EXPORT void TXTRecordDeallocate(txt_ref_t *record)
{
    if (record->txt) free(record->txt->bytes);
    free(record->txt);
    record->txt = NULL;
}
