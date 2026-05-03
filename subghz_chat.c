#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <lib/subghz/subghz_worker.h>
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
    SubGhzWorker* worker; // Добавили воркер для приема
    char tx_buf[64];
} ChatApp;

// Коллбэк воркера: вызывается при обнаружении любого сигнала в эфире
static void chat_rx_worker_callback(void* context) {
    ChatApp* app = context;
    // В полноценном чате здесь должен быть декодер протокола.
    // Пока просто сигнализируем в интерфейс, что что-то поймали.
    with_view_model(app->main_view, ChatModel* m, {
        strncpy(m->last_rx_msg, "Signal detected!", 63);
    }, true);
}

static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat (Unleashed)");
    
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 25, m->is_external ? "Ant: EXTERNAL" : "Ant: INTERNAL");
    canvas_draw_line(canvas, 0, 28, 128, 28);
    
    canvas_draw_str(canvas, 2, 42, "Last RX:");
    canvas_draw_str(canvas, 45, 42, m->last_rx_msg);
    canvas_draw_str(canvas, 2, 52, "Status: Listening...");
    canvas_draw_str(canvas, 2, 62, "OK: Write | UP/DN: Ant");
}

static void send_radio_packet(ChatApp* app) {
    // 1. Безопасная проверка частоты (чтобы не нарушать региональный лок)
    if(!furi_hal_subghz_is_tx_allowed(CHAT_FREQ)) return;

    // 2. Останавливаем воркер приема, чтобы монопольно занять радиомодуль
    if(subghz_worker_is_running(app->worker)) {
        subghz_worker_stop(app->worker);
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);

    // 3. ПЕРЕДАЧА: Чтобы не было Kernel Panic, используем прямой TX старт
    // В данном случае мы просто "светим" в эфир 100мс.
    // Для текста сюда нужно добавить encoder (библиотека subghz_protocol_encoder)
    if(furi_hal_subghz_tx_start()) {
        furi_delay_ms(100); 
        furi_hal_subghz_tx_stop();
    }
    
    furi_hal_subghz_idle();

    // 4. Возвращаемся в режим приема
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
                // Настройка антенн
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
    app->worker = subghz_worker_alloc(); // Инициализация воркера

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLockFree, sizeof(ChatModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, render_callback);
    view_set_input_callback(app->main_view, input_callback);
    view_set_previous_callback(app->main_view, prev_callback);

    app->text_input = text_input_alloc();
    text_input_set_result_callback(app->text_input, text_input_done, app, app->tx_buf, 64, true);
    text_input_set_header_text(app->text_input, "Message:");
    view_set_previous_callback(text_input_get_view(app->text_input), back_to_main_callback);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_add_view(app->view_dispatcher, 1, text_input_get_view(app->text_input));
    
    // Начальная настройка радио
    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    
    // Запускаем воркер приема
    subghz_worker_set_overrun_callback(app->worker, chat_rx_worker_callback);
    subghz_worker_start(app->worker);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    view_dispatcher_run(app->view_dispatcher);

    // Чистый выход: останавливаем воркер и выключаем радио
    if(subghz_worker_is_running(app->worker)) subghz_worker_stop(app->worker);
    subghz_worker_free(app->worker);
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
