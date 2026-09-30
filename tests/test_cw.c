/* Compiles the actual modem source, replacing only platform/UI dependencies. */
#define CW_UNIT_TEST 1
#include "../src/modem_cw.c"

static struct cw_config ui = {1, 2, CW_STRAIGHT, 20, 600, 300, 7020000, 7020000, 40};
static uint32_t clock_us;
static int radio_on, starts, stops;
static const char *typing = "";
static int was_voice;
static int get_tx_data_byte(char *c) {
    if(was_voice && !radio_on) return 0;
    if (!*typing) return 0;
    *c = *typing++; return 1;
}
void cw_ui_config(struct cw_config *c) { *c = ui; }
void cw_ui_tx_state(int on, int mode) { (void)on; (void)mode; }
int cw_radio_set(int on, const struct cw_config *c, unsigned e) {
    (void)c;
    if (on == 1) { assert(cw_runtime_valid(e)); radio_on=1; starts++; return 1; }
    if (on == 0 && !cw_runtime_can_stop(e)) return 0;
    cw_runtime_pause(); radio_on=0; stops++; return 1;
}
static void tick(int raw) {
    clock_us += 1000;
    cw_test_input(clock_us, raw);
}
static void begin(int method) {
    cw_runtime_disable();
    cw_test_control(clock_us);
    ui.enabled=1; ui.input_method=method;
    cw_poll(0,0);
    tick(0); tick(0); tick(0);
    cw_audio_begin();
}
static void event(int state) { tick(state); tick(state); }
static void start(void) { cw_test_control(clock_us); cw_audio_begin(); assert(radio_on); }

static void test_queue(void) {
    struct cw_event_queue q = {0}; struct cw_event e;
    for (int round=0; round<10; round++) {
        for (unsigned i=0;i<CW_EVENTS;i++) assert(cw_events_push(&q,(struct cw_event){i,1,(int)i}));
        assert(!cw_events_push(&q,(struct cw_event){0}));
        for (unsigned i=0;i<CW_EVENTS;i++) { assert(cw_events_peek(&q,&e)); assert(e.us==i); cw_events_pop(&q); }
        assert(!cw_events_peek(&q,&e));
    }
    atomic_store(&q.write, UINT32_MAX-1); atomic_store(&q.read, UINT32_MAX-1);
    for (int i=0;i<4;i++) assert(cw_events_push(&q,(struct cw_event){(unsigned)i,1,i}));
    for (int i=0;i<4;i++) { assert(cw_events_peek(&q,&e)); assert(e.state==i); cw_events_pop(&q); }
}
static void test_buffered_pulse(void) {
    begin(CW_STRAIGHT);
    event(CW_DOWN);
    /* Whole 10 ms pulse happened before the TX controller could prepare PA. */
    for(int i=0;i<8;i++) tick(CW_DOWN);
    event(0);
    start();
    int down=0;
    for(int i=0;i<1500;i++) { cw_tx_get_sample(); if(cw_key_state==CW_DOWN) down++; }
    assert(down==960);
    assert(cw_envelope_pos==0);
    puts("PASS queued down/up during TX preparation preserves 10 ms pulse");
}
static void test_rekey(void) {
    begin(CW_STRAIGHT);
    event(CW_DOWN); for(int i=0;i<8;i++) tick(CW_DOWN);
    event(0); event(CW_DOWN); /* 2 ms gap, shorter than envelope fall */
    start();
    for(int i=0;i<960;i++) cw_tx_get_sample();
    assert(cw_envelope_pos==480);
    cw_tx_get_sample(); assert(cw_envelope_pos==479);
    for(int i=0;i<191;i++) cw_tx_get_sample();
    int at_gap_end=cw_envelope_pos;
    cw_tx_get_sample();
    assert(cw_envelope_pos==at_gap_end+1);
    puts("PASS straight release on exact sample, re-key during fall without dead time");
}
static void test_dots(int mode) {
    begin(mode); event(CW_DOT); start();
    int onset[3], n=0, prev=0;
    for(int i=0;i<7*cw_period && n<3;i++) {
        cw_tx_get_sample();
        int rising=cw_envelope_pos > prev;
        if (rising && prev==0) onset[n++]=i;
        prev=cw_envelope_pos;
    }
    assert(n==3); assert(onset[1]-onset[0]==2*cw_period); assert(onset[2]-onset[1]==2*cw_period);
}
static void test_cancel_mode(void) {
    begin(CW_STRAIGHT); event(CW_DOWN); start();
    cw_tx_get_sample();
    cw_runtime_disable(); cw_test_control(clock_us);
    assert(!radio_on); assert(cw_tx_get_sample()==0);
    int old=starts;
    event(CW_DOWN); cw_test_control(clock_us); assert(starts==old);
    ui.enabled=1; cw_runtime_configure(&ui); cw_audio_begin();
    event(CW_DOWN); cw_test_control(clock_us); assert(starts==old);
    event(0); event(CW_DOWN); cw_test_control(clock_us); assert(starts==old+1);
    puts("PASS mode cancellation mutes immediately and held contact needs release");
}
static void test_no_second_block_reset(void) {
    begin(CW_STRAIGHT); cw_init(); cw_poll(0,0); tick(0); tick(0); tick(0);
    cw_audio_begin(); event(CW_DOWN); start();
    for(int i=0;i<1024;i++) cw_tx_get_sample();
    assert(cw_envelope_pos==480); cw_audio_begin(); assert(cw_envelope_pos==480);
}
static void test_timestamp_wrap(void) {
    clock_us=UINT32_MAX-5000;
    test_buffered_pulse();
    puts("PASS monotonic timestamp wrap");
}
static void test_squeeze_release(int mode, int expected) {
    begin(mode); event(CW_SQUEEZE);
    for(int i=0;i<8;i++) tick(CW_SQUEEZE);
    event(0); start();
    int n=0, prev=0;
    for(int i=0;i<40000;i++) {
        cw_tx_get_sample();
        if(cw_envelope_pos > prev && prev==0) n++;
        prev=cw_envelope_pos;
    }
    assert(n==expected);
}
static void test_text(void) {
    begin(CW_IAMBIC);
    was_voice=1;
    typing="ee"; cw_poll(2,0); start(); cw_poll(2,1);
    was_voice=0;
    int onset[2], n=0, prev=0;
    for(int i=0;i<30000;i++) {
        cw_tx_get_sample();
        if(cw_envelope_pos > prev && prev==0) { assert(n<2); onset[n++]=i; }
        prev=cw_envelope_pos;
    }
    assert(n==2); assert(onset[1]-onset[0]==4*5760);
    cw_poll(0,1);
    puts("PASS keyboard text and three-dit inter-character space");
    typing="tttt"; cw_poll(4,1);
    cw_abort(); cw_test_control(clock_us); cw_audio_begin(); cw_poll(0,0);
    int old=starts;
    tick(0); tick(0); cw_test_control(clock_us); assert(starts==old);
    puts("PASS abort discards queued text without re-starting TX");
}
static void test_hang_tail(void) {
    ui.hang_ms=0;
    begin(CW_STRAIGHT); event(CW_DOWN); event(0); start();
    for(int i=0;i<2000;i++) cw_tx_get_sample();
    cw_runtime_playback_frames(4096);
    cw_test_control(clock_us+20000); assert(radio_on);
    cw_test_control(clock_us+56000); assert(!radio_on);
    ui.hang_ms=300;
    puts("PASS PA remains on until queued audio tail can play, including zero hangtime");
}
static void test_overflow(void) {
    begin(CW_STRAIGHT);
    for(int i=0;i<260;i++) event(i%2 ? 0 : CW_DOWN);
    cw_test_control(clock_us); cw_audio_begin();
    assert(!radio_on); assert(cw_tx_get_sample()==0);
    puts("PASS queue overflow cancels TX and mutes");
}
int main(void) {
    test_queue(); puts("PASS FIFO order, full queue and index wrap");
    test_buffered_pulse(); test_rekey();
    for(int w=10;w<=40;w+=10) {
        ui.wpm=w;
        test_dots(CW_IAMBIC); test_dots(CW_IAMBICB); test_dots(CW_BUG); test_dots(CW_ULTIMATIC);
    }
    ui.wpm=20;
    puts("PASS 10/20/30/40 WPM dot spacing: iambic A/B, bug, ultimatic");
    test_squeeze_release(CW_IAMBIC,1); test_squeeze_release(CW_IAMBICB,2);
    puts("PASS squeeze release: mode A stops, mode B adds opposite element");
    test_text(); test_hang_tail();
    test_cancel_mode(); test_no_second_block_reset(); test_timestamp_wrap(); test_overflow();
    puts("ALL CW TESTS PASSED"); return 0;
}
