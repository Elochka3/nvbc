#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <string.h>

#define CHAT_FREQ 433920000 

// События для ViewDispatcher
typedef enum {
    ChatEventSendPacket,
} ChatCustomEvent;

// Сощения для очереди потока
typedef struct {
    char text[64];
} ChatMessage;

// Состояние энкодера для передачи битов
typedef struct {
    const char* data;
    size_t bit_idx;
    size_t byte_idx;
    size_t length;
    volatile bool is_done;
} TxEncoderState;

// Модель данных (состояние экрана)
typedef struct {
    char last_rx_msg[64];
    bool is_external;
} ChatModel;

// Главная структура приложения
typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    
    FuriThread* worker_thread;
    FuriMessageQueue* tx_queue;
    volatile bool is_running;
    
    char tx_buf[64];
} ChatApp;

static TxEncoderState tx_state;

// Коллбэк для генерации импульсов из текста (OOK Модуляция)
static LevelDuration chat_tx_callback(void* context) {
    UNUSED(context);
    
    if(tx_state.is_done) return level_duration_make(false, 1000);

    uint8_t byte = (uint8_t)tx_state.data[tx_state.byte_idx];
    bool bit = (byte >> (7 - tx_state.bit_idx)) & 1;

    // Тайминги: 1 = 500мкс сигнал, 0 = 200мкс сигнал
    uint32_t duration = bit ? 500 : 200;
    
    tx_state.bit_idx++;
    if(tx_state.bit_idx >= 8) {
        tx_state.bit_idx = 0;
        tx_state.byte_idx++;
        if(tx_state.byte_idx >= tx_state.length) {
            tx_state.is_done = true;
        }
    }

    return level_duration_make(true, duration);
}

// Поток для работы с радио (не блокирует GUI)
static int32_t chat_worker_thread(void* context) {
    ChatApp* app = context;
    ChatMessage msg;

    while(app->is_running) {
        if(furi_message_queue_get(app->tx_queue, &msg, 100) == FuriStatusOk) {
            // Подготовка энкодера
            tx_state.data = msg.text;
            tx_state.length = strlen(msg.text);
            tx_state.byte_idx = 0;
            tx_state.bit_idx = 0;
            tx_state.is_done = false;

            furi_hal_subghz_idle();
            furi_hal_subghz_set_frequency(CHAT_FREQ);
            
            if(furi_hal_subghz_start_async_tx(chat_tx_callback, NULL)) {
                // Ждем завершения передачи битов
                while(!tx_state.is_done && app->is_running) {
                    furi_delay_ms(10);
                }
                furi_delay_ms(50); 
                furi_hal_subghz_stop_async_tx();
            }
            furi_hal_subghz_rx();
        }
    }
    return 0;
}

static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat v4.1");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 24, m->is_external ? "Ant: EXTERNAL" : "Ant: INTERNAL");
    canvas_draw_line(canvas, 0, 26, 128, 26);
    canvas_draw_str(canvas, 2, 42, "Last RX:");
    canvas_draw_str(canvas, 45, 42, m->last_rx_msg);
    canvas_draw_str(canvas, 2, 62, "OK: Write | UP/DN: Ant");
}

static void send_radio_packet(ChatApp* app) {
    ChatMessage msg;
    strncpy(msg.text, app->tx_buf, 63);
    furi_message_queue_put(app->tx_queue, &msg, 0);
}

static bool chat_custom_event_callback(void* context, uint32_t event) {
    ChatApp* app = context;
    if(event == ChatEventSendPacket) {
        send_radio_packet(app);
        return true;
    }
    return false;
}

static void text_input_done(void* ctx) {
    ChatApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, ChatEventSendPacket);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
}

static uint32_t prev_callback(void* ctx) { UNUSED(ctx); return VIEW_NONE; }
static uint32_t back_to_main_callback(void* ctx) { UNUSED(ctx); return 0; }

static bool input_callback(InputEvent* event, void* ctx) {
    ChatApp* app = ctx;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            view_dispatcher_switch_to_view(app->view_dispatcher, 1);
            return true;
        } else if(event->key == InputKeyUp || event->key == InputKeyDown) {
            with_view_model(app->main_view, ChatModel * m, {
                m->is_external = !m->is_external;
                furi_hal_subghz_set_path(m->is_external ? 1 : 0);
            }, true);
            return true;
        }
    }
    return false;
}

int32_t subghz_chat_app(void* p) {
    UNUSED(p);
    ChatApp* app = malloc(sizeof(ChatApp));
    memset(app, 0, sizeof(ChatApp));
    app->is_running = true;

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    
    // Поток и очереди
    app->tx_queue = furi_message_queue_alloc(8, sizeof(ChatMessage));
    app->worker_thread = furi_thread_alloc_ex("ChatWorker", 1024, chat_worker_thread, app);
    furi_thread_start(app->worker_thread);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, chat_custom_event_callback);

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLockFree, sizeof(ChatModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, render_callback);
    view_set_input_callback(app->main_view, input_callback);
    view_set_previous_callback(app->main_view, prev_callback);

    app->text_input = text_input_alloc();
    text_input_set_result_callback(app->text_input, text_input_done, app, app->tx_buf, 64, true);
    view_set_previous_callback(text_input_get_view(app->text_input), back_to_main_callback);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_add_view(app->view_dispatcher, 1, text_input_get_view(app->text_input));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    furi_hal_subghz_rx();

    view_dispatcher_run(app->view_dispatcher);

    // Завершение работы
    app->is_running = false;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);
    furi_message_queue_free(app->tx_queue);

    furi_hal_subghz_idle();
    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_dispatcher_remove_view(app->view_dispatcher, 1);
    text_input_free(app->text_input);
    view_free(app->main_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
