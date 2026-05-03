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

// Максимально простая отрисовка для теста
static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat v4.6");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 30, m->last_rx_msg);
    canvas_draw_str(canvas, 2, 60, "OK: Send Pulse");
}

// Заглушка TX
static LevelDuration chat_tx_callback_dummy(void* context) {
    UNUSED(context);
    return level_duration_make(true, 500); 
}

static int32_t chat_worker_thread(void* context) {
    ChatApp* app = context;
    ChatMessage msg;
    while(app->is_running) {
        if(furi_message_queue_get(app->tx_queue, &msg, 100) == FuriStatusOk) {
            furi_hal_subghz_idle();
            furi_hal_subghz_set_frequency(CHAT_FREQ);
            if(furi_hal_subghz_start_async_tx(chat_tx_callback_dummy, NULL)) {
                furi_delay_ms(100);
                furi_hal_subghz_stop_async_tx();
            }
            furi_hal_subghz_rx();
        }
    }
    return 0;
}

// Функции-заглушки для ввода
static void text_input_done(void* ctx) {
    ChatApp* app = ctx;
    ChatMessage msg;
    strncpy(msg.text, "Pulse", 63);
    furi_message_queue_put(app->tx_queue, &msg, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
}
static uint32_t back_to_main(void* ctx) { UNUSED(ctx); return 0; }

int32_t subghz_chat_app(void* p) {
    UNUSED(p);
    ChatApp* app = malloc(sizeof(ChatApp));
    if(!app) return -1;
    memset(app, 0, sizeof(ChatApp));
    app->is_running = true;

    // 1. Сначала запускаем GUI
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    
    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLockFree, sizeof(ChatModel));
    view_set_draw_callback(app->main_view, render_callback);
    
    app->text_input = text_input_alloc();
    text_input_set_result_callback(app->text_input, text_input_done, app, app->tx_buf, 64, true);
    view_set_previous_callback(text_input_get_view(app->text_input), back_to_main);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_add_view(app->view_dispatcher, 1, text_input_get_view(app->text_input));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    // 2. Только ПОСЛЕ GUI инициализируем радио (очень осторожно)
    app->tx_queue = furi_message_queue_alloc(4, sizeof(ChatMessage));
    app->worker_thread = furi_thread_alloc_ex("ChatWrk", 1024, chat_worker_thread, app);
    furi_thread_start(app->worker_thread);

    // Включаем чип в режим приема
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    furi_hal_subghz_rx();

    // 3. Главный цикл
    view_dispatcher_run(app->view_dispatcher);

    // Чистим всё
    app->is_running = false;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);
    furi_message_queue_free(app->tx_queue);
    
    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_dispatcher_remove_view(app->view_dispatcher, 1);
    text_input_free(app->text_input);
    view_free(app->main_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
