int initvInputClient(const char []);
void send_sync_event();
void send_event(int32_t type, int32_t code, int32_t value);
bool vinput_mouse_position(uint32_t x, uint32_t y);
bool vinput_touch_release(u_int32_t x, u_int32_t y);
bool vinput_touch_press(u_int32_t x, u_int32_t y);
