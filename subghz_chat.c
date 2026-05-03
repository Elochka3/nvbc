#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <string.h>

#include <lib/subghz/receiver.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/protocols/base.h>

#define CHAT_FREQ 433920000 

typedef enum {
    ChatEventSendPacket,
} ChatCustomEvent;

typedef struct {
    char text[64];
} ChatMessage;

typedef struct {
    char last_rx_msg[64];
    bool is_external;
} ChatModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    FuriThread* worker_thread;
    FuriMessageQueue* tx_queue;
    volatile bool is_running;
    SubGhzEnvironment* env;
    SubGhzReceiver* receiver;
    SubGhzWorker* subghz_worker;
    char tx_buf[64];
} ChatApp;

// Заглушка, чтобы async_tx не падал в Kernel Panic
static LevelDuration chat_tx_callback_dummy(void* context) {
    UNUSED(context);
    return level_duration_make(true, 500); 
}

static void chat_receiver_callback(SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* decoder, void* context) {
    ChatApp* app = context;
    UNUSED(receiver);
    FuriString* res = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder, res);
    with_view_model(app->main_view, ChatModel* m, {
        strncpy(m->last_rx_msg, furi_string_get_cstr(res), 63);
    }, true);
    furi_string_free(res);
}

static void chat_subghz_worker_callback(void* context, bool level, uint32_t duration) {
    ChatApp* app = context;
    if(app && app->receiver) {
        subghz_receiver_decode(app->receiver, level, duration);
    }
}

static int32_t chat_worker_thread(void* context) {
    ChatApp* app = context;
    ChatMessage msg;
    while(app->is_running) {
        if(furi_message_queue_get(app->tx_queue, &msg, 100) == FuriStatusOk) {
            if(subghz_worker_is_running(app->subghz_worker)) subghz_worker_stop(app->subghz_worker);
            
            furi_hal_subghz_idle();
            furi_hal_subghz_set_frequency(CHAT_FREQ);
            
            // Включаем безопасную передачу импульса
            if(furi_hal_subghz_start_async_tx(chat_tx_callback_dummy, NULL)) {
                furi_delay_ms(150);
                furi_hal_subghz_stop_async_tx();
            }
            
            furi_hal_subghz_rx();
            subghz_worker_start(app->subghz_worker);
        }
    }
    return 0;
}

static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat v4.5");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 24, m->is_external ? "Ant: EXTERNAL" : "Ant: INTERNAL");
    canvas_draw_line(canvas, 0, 26, 128, 26);
    canvas_draw_str(canvas, 2, 42, "RX Status:");
    canvas_draw_str(canvas, 60, 42, m->last_rx_msg);
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
    
    // Инициализация радио
    app->env = subghz_environment_alloc();
    subghz_environment_load_keystore(app->env, "any");
    app->receiver = subghz_receiver_alloc_init(app->env);
    subghz_receiver_set_rx_callback(app->receiver, chat_receiver_callback, app);
    
    app->subghz_worker = subghz_worker_alloc();
    subghz_worker_set_context(app->subghz_worker, app);
    subghz_worker_set_pair_callback(app->subghz_worker, (SubGhzWorkerPairCallback)chat_subghz_worker_callback);

    app->tx_queue = furi_message_queue_alloc(8, sizeof(ChatMessage));
    app->worker_thread = furi_thread_alloc_ex("ChatWorker", 2048, chat_worker_thread, app);
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
    subghz_worker_start(app->subghz_worker);

    view_dispatcher_run(app->view_dispatcher);

    app->is_running = false;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);
    furi_message_queue_free(app->tx_queue);
    
    if(subghz_worker_is_running(app->subghz_worker)) subghz_worker_stop(app->subghz_worker);
    subghz_worker_free(app->subghz_worker);
    subghz_receiver_free(app->receiver);
    subghz_environment_free(app->env);

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
