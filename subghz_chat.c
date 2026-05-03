#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <string.h>

#define CHAT_FREQ 433920000 

typedef struct {
    char text[64];
} ChatMessage;

typedef struct {
    char last_msg[64];
} ChatModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    FuriThread* worker_thread;
    FuriMessageQueue* tx_queue;
    volatile bool is_running;
} ChatApp;

// Коллбэк для генерации импульса (безопасный)
static LevelDuration chat_tx_callback_dummy(void* context) {
    UNUSED(context);
    return level_duration_make(true, 500); 
}

// Рендер экрана
static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat v4.7");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 35, m->last_msg);
    canvas_draw_str(canvas, 2, 60, "Press OK to Send Pulse");
}

// ПОТОК РАДИО: работает отдельно, чтобы не фризить GUI
static int32_t chat_worker_thread(void* context) {
    ChatApp* app = context;
    ChatMessage msg;
    
    while(app->is_running) {
        // Ждем сообщения из очереди (таймаут 100мс)
        if(furi_message_queue_get(app->tx_queue, &msg, 100) == FuriStatusOk) {
            
            // Безопасный цикл TX
            furi_hal_subghz_idle();
            furi_delay_ms(10); // Даем чипу "продышаться"
            
            furi_hal_subghz_set_frequency(CHAT_FREQ);
            
            if(furi_hal_subghz_start_async_tx(chat_tx_callback_dummy, NULL)) {
                furi_delay_ms(150); // Длительность сигнала
                furi_hal_subghz_stop_async_tx();
            }
            
            furi_delay_ms(10);
            furi_hal_subghz_rx(); // Возвращаемся в прием
        }
        furi_thread_yield(); // Отдаем квант времени другим потокам
    }
    return 0;
}

// Обработка кнопок
static bool input_callback(InputEvent* event, void* ctx) {
    ChatApp* app = ctx;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        ChatMessage msg;
        memset(&msg, 0, sizeof(ChatMessage));
        
        // Обновляем текст в модели
        with_view_model(app->main_view, ChatModel* m, {
            strncpy(m->last_msg, "TX: Sending Pulse...", 63);
        }, true);

        // Кидаем команду в очередь воркера
        furi_message_queue_put(app->tx_queue, &msg, 0);
        return true;
    }
    return false;
}

int32_t subghz_chat_app(void* p) {
    UNUSED(p);
    ChatApp* app = malloc(sizeof(ChatApp));
    memset(app, 0, sizeof(ChatApp));
    app->is_running = true;

    // 1. GUI
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLockFree, sizeof(ChatModel));
    view_set_draw_callback(app->main_view, render_callback);
    view_set_input_callback(app->main_view, input_callback);
    view_set_context(app->main_view, app);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    // 2. РАДИО И ПОТОКИ
    app->tx_queue = furi_message_queue_alloc(2, sizeof(ChatMessage));
    app->worker_thread = furi_thread_alloc_ex("ChatWrk", 1024, chat_worker_thread, app);
    furi_thread_start(app->worker_thread);

    // Инициализация чипа один раз при старте
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    furi_hal_subghz_rx();

    // 3. RUN
    view_dispatcher_run(app->view_dispatcher);

    // 4. CLEANUP
    app->is_running = false;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);
    furi_message_queue_free(app->tx_queue);
    
    furi_hal_subghz_idle();
    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_free(app->main_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
