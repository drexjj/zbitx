void key_draw(struct field *f);
void keyboard_redraw();
char keyboard_read(field *key);
void keyboard_show(uint8_t mode);
void keyboard_hide();
char read_key();
void field_blink(int blink_state);
void text_draw(struct field *f);
void text_input(struct field *key);
