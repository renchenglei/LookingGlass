#ifndef __GUEST_CLIPBOARD_H_
#define __GUEST_CLIPBOARD_H_

typedef void (*GuestClipboardNotice )(const SpiceDataType type);
typedef void (*GuestClipboardData   )(const SpiceDataType type, uint8_t * buffer, uint32_t size);
typedef void (*GuestClipboardRelease)();
typedef void (*GuestClipboardRequest)(const SpiceDataType type);

bool guest_clipboard_prcess();

bool guest_set_clipboard_cb(
    GuestClipboardNotice  cbNoticeFn,
    GuestClipboardData    cbDataFn,
    GuestClipboardRelease cbReleaseFn,
    GuestClipboardRequest cbRequestFn);

bool guest_clipboard_request(SpiceDataType type);
bool guest_clipboard_grab(SpiceDataType type);
bool guest_clipboard_data(SpiceDataType type, uint8_t* data, size_t size);
bool guest_clipboard_release();

bool vsock_process();

bool vsock_connect();
bool vsock_read();
bool vsock_write();
bool vsock_disconnect();
bool vsock_disconnect_client();
bool vsock_connected();

void vsock_clear_buffers();
#endif
