#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/vm_sockets.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>

#include "spice/spice.h"
#include "common/debug.h"
#include "vsock_clipboard.h"

#define VMADDR_CID_HOST 2
#define VSOCK_PORT 77777
#define DATA_SIZE_LENGTH 4
#define MAX_CHUNK_LENGTH 8192
#define MAX_DATA_LENGTH 512*1024

struct VSock {
  int  _fd;
  int  _client_fd;
  bool _bind_success;
  size_t  _clipSize;
  uint8_t *_clipBuffer;
  uint8_t _defaultBuffer[MAX_CHUNK_LENGTH];
  GuestClipboardNotice  cbNoticeFn;
  GuestClipboardData    cbDataFn;
  GuestClipboardRelease cbReleaseFn;
  GuestClipboardRequest cbRequestFn;
};

struct VSock vsock =
{
  ._fd = 0,
  ._client_fd = 0,
  ._bind_success = false,
  ._clipSize = 0,
  ._clipBuffer = 0,
  ._defaultBuffer[0] = 0,
};

bool guest_set_clipboard_cb(
    GuestClipboardNotice  cbNoticeFn,
    GuestClipboardData    cbDataFn,
    GuestClipboardRelease cbReleaseFn,
    GuestClipboardRequest cbRequestFn) {
  if((cbNoticeFn && !cbDataFn) || (cbDataFn && !cbNoticeFn)) {
    DEBUG_ERROR("clipboard callback notice and data callbacks must be specified");
    return false;
  }

  vsock.cbNoticeFn  = cbNoticeFn;
  vsock.cbDataFn    = cbDataFn;
  vsock.cbReleaseFn = cbReleaseFn;
  vsock.cbRequestFn = cbRequestFn;

  return true;
}

bool vsock_connect() {
  if (vsock._fd <= 0) {
    vsock._fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (vsock._fd <= 0) {
      DEBUG_ERROR("failed to create socket");
      return false;
    }
  }

  struct sockaddr_vm sock_addr;
  memset(&sock_addr, 0, sizeof(struct sockaddr_vm));
  sock_addr.svm_family = AF_VSOCK;
  sock_addr.svm_port = VSOCK_PORT;
  sock_addr.svm_cid = VMADDR_CID_HOST;

  if (!vsock._bind_success && bind(vsock._fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) < 0) {
    DEBUG_ERROR("failed to bind to socket:%d", errno);
    return false;
  }
  vsock._bind_success = true;
  if (listen(vsock._fd, 1) < 0) {
    DEBUG_ERROR("failed to listen");
    return false;
  }

  if (!vsock_connected())
  {
    int flags = fcntl(vsock._fd, F_GETFL);
    fcntl(vsock._fd, F_SETFL, flags | O_NONBLOCK);
    size_t sock_addr_len = sizeof(sock_addr);
    if((vsock._client_fd = accept(vsock._fd, (struct sockaddr*)&sock_addr, (socklen_t*)&sock_addr_len)) < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        sleep(1);
      } else {
        DEBUG_ERROR("failed to accept connection");
      }
      return false;
    }
    fcntl(vsock._fd, F_SETFL, flags);
    if (vsock_connected()) {
      DEBUG_ERROR("Connected to client");
    }
  }
  return true;
}

bool guest_clipboard_request(SpiceDataType type) {
  if (type == SPICE_DATA_TEXT && vsock.cbDataFn && vsock._clipSize > 0) {
    vsock.cbDataFn(type, vsock._clipBuffer, vsock._clipSize);
    return true;
  }
  return false;
}

bool guest_clipboard_grab(SpiceDataType type) {
  if (type == SPICE_DATA_TEXT && vsock.cbRequestFn) {
    vsock_clear_buffers();
    vsock.cbRequestFn(type);
    return true; 
  }
  return false;
}

bool guest_clipboard_data(SpiceDataType type, uint8_t* data, size_t size) {
  if (type == SPICE_DATA_TEXT) {
    return vsock_write(data, size);
  }
  return false;
}

bool guest_clipboard_release() {
  return true;
}

bool vsock_process() {
  if (!vsock_connected()) {
    return false;
  }
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(vsock._client_fd, &readSet);

  struct timeval timeout;
  timeout.tv_sec  = 1;
  timeout.tv_usec = 0;

  int rc = select(FD_SETSIZE, &readSet, NULL, NULL, &timeout);
  if (rc < 0)
  {
    DEBUG_ERROR("select failure");
    return false;
  }

  if (rc  > 0) {
    return vsock_read();
  }
  return true;
}

int read_from_vsock(uint8_t* data, uint32_t size) {
  if (!vsock_connected()) {
    return false;
  }

  int nread = recv(vsock._client_fd, data, size, 0);
  if (nread <= 0) {
    if (nread == 0 || errno == ECONNRESET) {
      DEBUG_ERROR("Client connection reset");
    } else {
      DEBUG_ERROR("Failed to recieve message. error-%d", errno);
    }
    // On any errors its better to reset the connection so as to not violate our protocol
    // of sending the size followed by chunks of content 
    vsock_disconnect_client();
    return nread;
  }

  // We precisely know how much to read
  // This is a failure, which can lead to more problems later
  // This cannot happend and we cannot tolerate this
  //if (nread != size) {
  //  DEBUG_ERROR("Unexpected size of data");
  //  vsock_disconnect_client();
  //  return false;
  //}
  return nread;
}

bool vsock_read() {
  // Read a complete message and notify
  // First read the length of the data to recieve
  uint8_t data[DATA_SIZE_LENGTH];

  if (read_from_vsock(data, DATA_SIZE_LENGTH) != DATA_SIZE_LENGTH) {
    return false; 
  }

  uint32_t *size = (uint32_t*)data;
  uint32_t len = ntohl(*size);
  *size = len;
  DEBUG_INFO("Reading data of size=%d", *size);

  if (*size > MAX_DATA_LENGTH) {
    DEBUG_ERROR("Data too long");
    // This cannot be tolerated
    vsock_disconnect_client();
    return false;
  }

  vsock_clear_buffers();

  if (*size > MAX_CHUNK_LENGTH) {
    vsock._clipBuffer = (uint8_t*)malloc(*size + 1);
  }

  uint8_t* bytes = vsock._clipBuffer;
  int remaining = *size;
  uint8_t* new_bytes = bytes;
 
  while (remaining > 0) {
    int to_read = ((remaining > MAX_CHUNK_LENGTH) ? MAX_CHUNK_LENGTH : remaining);
    int nread = 0;
    if ((nread = read_from_vsock(new_bytes, to_read)) <= 0) {
      return false;
    }
    remaining -= nread;
    new_bytes += nread;
  }

  bytes[*size] = 0;
  vsock._clipSize = *size;

  if (vsock.cbNoticeFn) {
    vsock.cbNoticeFn(SPICE_DATA_TEXT);
  }
  return true;
}

bool vsock_disconnect() {
  vsock_disconnect_client();
  vsock_clear_buffers();
  close(vsock._fd);
  vsock._fd = 0; 
  return true;
}

bool vsock_disconnect_client() {
  if (vsock._client_fd) {
    close(vsock._client_fd);
    vsock._client_fd = 0;
  }
  return true;
}

bool write_to_vsock(uint8_t* bytes, uint32_t size) {
  if (!vsock_connected()) {
    return false;
  }

  if (send(vsock._client_fd, bytes, size, 0) != size) {
    DEBUG_ERROR("Failed to write complete data to client");
    // Cannot tolerate this
    vsock_disconnect_client();
    return false;
  }
  return true;
}

bool vsock_write(uint8_t *bytes, uint32_t size) {
  if (size > MAX_DATA_LENGTH) {
    DEBUG_INFO("Data too long... Truncating to %d", MAX_DATA_LENGTH);
    size = MAX_DATA_LENGTH;
  }
  DEBUG_INFO("Writing data size=%d", size);
  int32_t conv_size = htonl(size);
  uint8_t* data = (uint8_t*)&conv_size;
  if (!write_to_vsock(data, DATA_SIZE_LENGTH)) {
    DEBUG_ERROR("Failed to write to server");
    return false;
  }

  while (size > 0) {
    uint32_t size_to_send = (size > MAX_CHUNK_LENGTH) ? MAX_CHUNK_LENGTH : size;
    if (!write_to_vsock(bytes, size_to_send)) {
      DEBUG_ERROR("Failed to write to server");
      return false;
    }
    size -= size_to_send;
    bytes += size_to_send;
  }

  return true;
}

void vsock_clear_buffers() {
  vsock._clipSize = 0;
  if (vsock._clipBuffer &&
      vsock._clipBuffer != vsock._defaultBuffer) {
    free(vsock._clipBuffer);
  }
  vsock._clipBuffer = vsock._defaultBuffer;
  vsock._defaultBuffer[0] = 0;
}

bool vsock_connected() {
  return vsock._client_fd > 0;
}

