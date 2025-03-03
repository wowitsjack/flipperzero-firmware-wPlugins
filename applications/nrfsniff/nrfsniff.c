#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <furi_hal_gpio.h>
#include <furi_hal_spi.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_resources.h>
#include <nrf24.h>
#include <toolbox/stream/file_stream.h>

#define LOGITECH_MAX_CHANNEL 85
#define COUNT_THRESHOLD 4
#define SAMPLE_TIME 20000

#define NRFSNIFF_APP_PATH_FOLDER "/ext/nrfsniff"
#define NRFSNIFF_APP_FILENAME "addresses.txt"
#define TAG "nrfsniff"

typedef enum {
    EventTypeTick,
    EventTypeKey,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} PluginEvent;

typedef struct {
    int x;
    int y;
} PluginState;

char rate_text_fmt[] = "Transfer rate: %dMbps";
char channel_text_fmt[] = "Channel: %d";
char preamble_text_fmt[] = "Preamble: %02X";
char sniff_text_fmt[] = "Sniffing: %s";
char addresses_header_text[] = "Address,rate";
char sniffed_address_fmt[] = "%s,%d";
char rate_text[46];
char channel_text[42];
char preamble_text[14];
char sniff_text[38];
char sniffed_address[14];

uint8_t target_channel = 0;
uint8_t target_rate = 8; // rate can be either 8 (2Mbps) or 0 (1Mbps)
uint8_t target_preamble[] = {0xAA, 0x00};
uint8_t sniffing_state = false;
char top_address[12];

uint8_t candidates[100][5] = {0}; // top 100 recurring addresses
uint32_t counts[100];
uint8_t total_candidates = 0;
uint8_t last_cleanup_idx = 101; // avoid replacing the last replaced addr

static int get_addr_index(uint8_t* addr, uint8_t addr_size) {
    for(int i = 0; i < total_candidates; i++) {
        uint8_t* arr_item = candidates[i];
        if(!memcmp(arr_item, addr, addr_size)) return i;
    }

    return -1;
}

static uint32_t get_addr_count(uint8_t* addr, uint8_t addr_size) {
    return counts[get_addr_index(addr, addr_size)];
}

static uint8_t get_lowest_idx() {
    uint32_t lowest = 10000;
    uint8_t lowest_idx = 0;
    for(uint8_t i = 0; i < total_candidates; i++) {
        if(i == last_cleanup_idx) continue;

        if(counts[i] < lowest) {
            lowest = counts[i];
            lowest_idx = i;
        }
    }
    last_cleanup_idx = lowest_idx;
    return lowest_idx;
}

static uint8_t get_highest_idx() {
    uint32_t highest = 0;
    uint8_t highest_idx = 0;
    for(uint8_t i = 0; i < total_candidates; i++) {
        if(counts[i] > highest) {
            highest = counts[i];
            highest_idx = i;
        }
    }

    return highest_idx;
}

static void insert_addr(uint8_t* addr, uint8_t addr_size) {
    uint8_t idx = total_candidates;
    if(total_candidates > 99) {
        // replace addr with lowest count
        idx = get_lowest_idx();
    }
    memcpy(candidates[idx], addr, addr_size);
    counts[idx] = 1;
    if(total_candidates < 100) total_candidates++;
}

static void render_callback(Canvas* const canvas, void* ctx) {
    uint8_t rate = 2;
    char sniffing[] = "Yes";

    const PluginState* plugin_state = acquire_mutex((ValueMutex*)ctx, 25);
    if(plugin_state == NULL) {
        return;
    }
    // border around the edge of the screen
    canvas_draw_frame(canvas, 0, 0, 128, 64);
    canvas_set_font(canvas, FontSecondary);

    if(target_rate == 0) rate = 1;

    if(!sniffing_state) strcpy(sniffing, "No");

    snprintf(rate_text, sizeof(rate_text), rate_text_fmt, (int)rate);
    snprintf(channel_text, sizeof(channel_text), channel_text_fmt, (int)target_channel);
    snprintf(preamble_text, sizeof(preamble_text), preamble_text_fmt, target_preamble[0]);
    snprintf(sniff_text, sizeof(sniff_text), sniff_text_fmt, sniffing);
    snprintf(
        sniffed_address, sizeof(sniffed_address), sniffed_address_fmt, top_address, (int)rate);
    canvas_draw_str_aligned(canvas, 10, 10, AlignLeft, AlignBottom, rate_text);
    canvas_draw_str_aligned(canvas, 10, 20, AlignLeft, AlignBottom, channel_text);
    canvas_draw_str_aligned(canvas, 10, 30, AlignLeft, AlignBottom, preamble_text);
    canvas_draw_str_aligned(canvas, 10, 40, AlignLeft, AlignBottom, sniff_text);
    canvas_draw_str_aligned(canvas, 30, 50, AlignLeft, AlignBottom, addresses_header_text);
    canvas_draw_str_aligned(canvas, 30, 60, AlignLeft, AlignBottom, sniffed_address);

    release_mutex((ValueMutex*)ctx, plugin_state);
}

static void input_callback(InputEvent* input_event, FuriMessageQueue* event_queue) {
    furi_assert(event_queue);

    PluginEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void hexlify(uint8_t* in, uint8_t size, char* out) {
    memset(out, 0, size * 2);
    for(int i = 0; i < size; i++)
        snprintf(out + strlen(out), sizeof(out + strlen(out)), "%02X", in[i]);
}

static bool save_addr_to_file(Storage* storage, uint8_t* data, uint8_t size) {
    size_t file_size = 0;
    uint8_t linesize = 0;
    char filepath[42] = {0};
    char addrline[14] = {0};
    char ending[4];
    uint8_t* file_contents;
    uint8_t rate = 1;
    Stream* stream = file_stream_alloc(storage);

    if(target_rate == 8) rate = 2;
    snprintf(ending, sizeof(ending), ",%d\n", rate);
    hexlify(data, size, addrline);
    strcat(addrline, ending);
    linesize = strlen(addrline);
    strcpy(filepath, NRFSNIFF_APP_PATH_FOLDER);
    strcat(filepath, "/");
    strcat(filepath, NRFSNIFF_APP_FILENAME);
    stream_seek(stream, 0, StreamOffsetFromStart);

    // check if address already exists in file
    if(file_stream_open(stream, filepath, FSAM_READ, FSOM_OPEN_EXISTING)) {
        bool found = false;
        file_size = stream_size(stream);
        stream_seek(stream, 0, StreamOffsetFromStart);
        if(file_size > 0) {
            file_contents = malloc(file_size + 1);
            memset(file_contents, 0, file_size + 1);
            if(stream_read(stream, file_contents, file_size) > 0) {
                char* line = strtok((char*)file_contents, "\n");

                while(line != NULL) {
                    if(!memcmp(line, addrline, 12)) {
                        found = true;
                        break;
                    }
                    line = strtok(NULL, "\n");
                }
            }
            free(file_contents);
        }
        stream_free(stream);
        if(found) return false;

        stream = file_stream_alloc(storage);
        stream_seek(stream, 0, StreamOffsetFromStart);
    }

    // save address to file
    if(!file_stream_open(stream, filepath, FSAM_WRITE, FSOM_OPEN_APPEND))
        FURI_LOG_I(TAG, "Cannot open file \"%s\"", filepath);
    if(stream_write(stream, (uint8_t*)addrline, linesize) != linesize)
        FURI_LOG_I(TAG, "failed to write bytes to file stream");

    FURI_LOG_I(TAG, "save successful");
    stream_free(stream);
    return true;
}

void alt_address(uint8_t* addr, uint8_t* altaddr) {
    uint8_t macmess_hi_b[4];
    uint32_t macmess_hi;
    uint8_t macmess_lo;
    uint8_t preserved;
    uint8_t tmpaddr[5];

    // swap bytes
    for(int i = 0; i < 5; i++) tmpaddr[i] = addr[4 - i];

    // get address into 32-bit and 8-bit variables
    memcpy(macmess_hi_b, tmpaddr, 4);
    macmess_lo = tmpaddr[4];

    macmess_hi = bytes_to_int32(macmess_hi_b, true);

    //preserve lowest bit from hi to shift to low
    preserved = macmess_hi & 1;
    macmess_hi >>= 1;
    macmess_lo >>= 1;
    macmess_lo = (preserved << 7) | macmess_lo;
    int32_to_bytes(macmess_hi, macmess_hi_b, true);
    memcpy(tmpaddr, macmess_hi_b, 4);
    tmpaddr[4] = macmess_lo;

    // swap bytes back
    for(int i = 0; i < 5; i++) altaddr[i] = tmpaddr[4 - i];
}

static void wrap_up(Storage* storage) {
    uint8_t ch;
    uint8_t addr[5];
    uint8_t altaddr[5];
    char trying[12];
    uint8_t idx;
    uint8_t rate = 0;
    if(target_rate == 8) rate = 2;

    nrf24_set_idle(nrf24_HANDLE);

    while(true) {
        idx = get_highest_idx();
        if(counts[idx] < COUNT_THRESHOLD) break;

        counts[idx] = 0;
        memcpy(addr, candidates[idx], 5);
        hexlify(addr, 5, trying);
        FURI_LOG_I(TAG, "trying address %s", trying);
        ch = nrf24_find_channel(nrf24_HANDLE, addr, addr, 5, rate, 2, LOGITECH_MAX_CHANNEL, false);
        FURI_LOG_I(TAG, "find_channel returned %d", (int)ch);
        if(ch > LOGITECH_MAX_CHANNEL) {
            alt_address(addr, altaddr);
            hexlify(altaddr, 5, trying);
            FURI_LOG_I(TAG, "trying alternate address %s", trying);
            ch = nrf24_find_channel(
                nrf24_HANDLE, altaddr, altaddr, 5, rate, 2, LOGITECH_MAX_CHANNEL, false);
            FURI_LOG_I(TAG, "find_channel returned %d", (int)ch);
            memcpy(addr, altaddr, 5);
        }

        if(ch <= LOGITECH_MAX_CHANNEL) {
            hexlify(addr, 5, top_address);
            save_addr_to_file(storage, addr, 5);
            break;
        }
    }
}

static void start_sniffing() {
    memset(candidates, 0, sizeof(candidates));
    memset(counts, 0, sizeof(counts));
    nrf24_init_promisc_mode(nrf24_HANDLE, target_channel, target_rate);
}

int32_t nrfsniff_app(void* p) {
    uint8_t address[5] = {0};
    uint32_t start = 0;
    hexlify(address, 5, top_address);
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(PluginEvent));
    PluginState* plugin_state = malloc(sizeof(PluginState));
    ValueMutex state_mutex;
    if(!init_mutex(&state_mutex, plugin_state, sizeof(PluginState))) {
        FURI_LOG_E(TAG, "cannot create mutex\r\n");
        free(plugin_state);
        return 255;
    }

    nrf24_init();

    // Set system callbacks
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, &state_mutex);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open("gui");
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    Storage* storage = furi_record_open("storage");
    storage_common_mkdir(storage, NRFSNIFF_APP_PATH_FOLDER);

    PluginEvent event;
    for(bool processing = true; processing;) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 100);
        PluginState* plugin_state = (PluginState*)acquire_mutex_block(&state_mutex);

        if(event_status == FuriStatusOk) {
            // press events
            if(event.type == EventTypeKey) {
                if(event.input.type == InputTypePress ||
                   (event.input.type == InputTypeLong && event.input.key == InputKeyBack)) {
                    switch(event.input.key) {
                    case InputKeyUp:
                        // toggle rate  1/2Mbps
                        if(!sniffing_state) {
                            if(target_rate == 0)
                                target_rate = 8;
                            else
                                target_rate = 0;
                        }
                        break;
                    case InputKeyDown:
                        // toggle preamble
                        if(!sniffing_state) {
                            if(target_preamble[0] == 0x55)
                                target_preamble[0] = 0xAA;
                            else
                                target_preamble[0] = 0x55;

                            nrf24_set_src_mac(nrf24_HANDLE, target_preamble, 2);
                        }
                        break;
                    case InputKeyRight:
                        // increment channel
                        if(!sniffing_state && target_channel <= LOGITECH_MAX_CHANNEL)
                            target_channel++;
                        break;
                    case InputKeyLeft:
                        // decrement channel
                        if(!sniffing_state && target_channel > 0) target_channel--;
                        break;
                    case InputKeyOk:
                        // toggle sniffing
                        sniffing_state = !sniffing_state;
                        if(sniffing_state) {
                            start_sniffing();
                            start = furi_get_tick();
                        } else
                            wrap_up(storage);
                        break;
                    case InputKeyBack:
                        if(event.input.type == InputTypeLong) processing = false;
                        break;
                    }
                }
            }
        } else {
            FURI_LOG_D(TAG, "osMessageQueue: event timeout");
            // event timeout
        }

        if(sniffing_state) {
            if(nrf24_sniff_address(nrf24_HANDLE, 5, address)) {
                int idx;
                uint8_t* top_addr;
                idx = get_addr_index(address, 5);
                if(idx == -1)
                    insert_addr(address, 5);
                else
                    counts[idx]++;

                top_addr = candidates[get_highest_idx()];
                hexlify(top_addr, 5, top_address);
            }

            if(furi_get_tick() - start >= SAMPLE_TIME) {
                wrap_up(storage);
                target_channel++;
                if(target_channel > LOGITECH_MAX_CHANNEL) target_channel = 2;

                start_sniffing();
                start = furi_get_tick();
            }
        }

        view_port_update(view_port);
        release_mutex(&state_mutex, plugin_state);
    }

    furi_hal_spi_release(nrf24_HANDLE);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close("gui");
    furi_record_close("storage");
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    return 0;
}
