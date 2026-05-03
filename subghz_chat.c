#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/protocols/raw_generic.h>
#include <string.h>

#define CHAT_FREQ 433920000 

typedef struct {
    char last_rx_msg[64];
    bool is_external;
} ChatModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    
    SubGhzWorker* worker;
    SubGhzReceiver* receiver;
    SubGhzEnvironment* env;
    
    char tx_buf[64];
} ChatApp;

// Коллбэк приема данных: вызывается, когда декодер узнал протокол
static void chat_rx_callback(SubGhzReceiver* receiver, SubGhzProtocolDecoder* decoder, void* context) {
    UNUSED(receiver);
    ChatApp* app = context;
    
    with_view_model(app->main_view, ChatModel* m, {
        // Получаем имя протокола или данные (в упрощенном виде для теста)
        snprintf(m->last_rx_msg, 64, "Msg: %s", subghz_protocol_decoder_get_name(decoder));
    }, true);
}

static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat v1.0");
    
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 24, m->is_external ? "Ant: EXTERNAL" : "Ant: INTERNAL");
    canvas_draw_line(canvas, 0, 26, 128, 26);
    
    canvas_draw_str(canvas, 2, 40, "Last Message:");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 52, m->last_rx_msg);
    
    canvas_draw_str(canvas, 2, 62, "OK: Send | UP/DN: Ant");
}

static void send_radio_packet(ChatApp* app) {
    if(!furi_hal_subghz_is_tx_allowed(CHAT_FREQ)) return;

    if(subghz_worker_is_running(app->worker)) subghz_worker_stop(app->worker);

    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency(CHAT_FREQ);

    // В полноценном режиме мы используем генерацию пакета
    // Для простоты здесь имитация передачи (несущая)
    if(furi_hal_subghz_start_async_tx(NULL, NULL)) {
        furi_delay_ms(100);
        furi_hal_subghz_stop_async_tx();
    }
    
    furi_hal_subghz_idle();
    subghz_worker_start(app->worker);
}

static void text_input_done(void* ctx) {
    ChatApp* app = ctx;
    send_radio_packet(app);
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
                furi_hal_subghz_set_path(m->is_external ? FuriHalSubGhzPathIsolate : FuriHalSubGhzPathMain);
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
    
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    
    // Инициализация радио-стека
    app->env = subghz_environment_alloc();
    subghz_environment_load_all_protocols(app->env);
    app->receiver = subghz_receiver_alloc_init(app->env);
    subghz_receiver_set_rx_callback(app->receiver, chat_rx_callback, app);
    
    app->worker = subghz_worker_alloc();
    subghz_worker_set_overrun_callback(app->worker, (SubGhzWorkerOverrunCallback)subghz_receiver_decode, app->receiver);

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
    
    // Настройка RX
    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    subghz_worker_start(app->worker);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    view_dispatcher_run(app->view_dispatcher);

    // Очистка
    subghz_worker_stop(app->worker);
    subghz_worker_free(app->worker);
    subghz_receiver_free(app->receiver);
    subghz_environment_free(app->env);
    
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_dispatcher_remove_view(app->view_dispatcher, 1);
    text_input_free(app->text_input);
    view_free(app->main_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
