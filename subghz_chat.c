#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <string.h>

#define CHAT_FREQ 433920000 

typedef enum {
    ChatStateIdle,
    ChatStateSending,
} ChatState;

typedef struct {
    char text[64]; 
} ChatMessage;

typedef struct {
    char last_msg[64];
    ChatState state;
} ChatModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    FuriThread* worker_thread;
    FuriMessageQueue* tx_queue;
    volatile bool is_running;
    volatile ChatState current_state;
    char tx_buf[64];
} ChatApp;

// Тестовый импульс
static LevelDuration chat_tx_callback_payload(void* context) {
    UNUSED(context);
    return level_duration_make(true, 500); 
}

// Поток радио (отделен от GUI)
static int32_t chat_worker_thread(void* context) {
    ChatApp* app = context;
    ChatMessage msg;

    while(app->is_running) {
        if(furi_message_queue_get(app->tx_queue, &msg, 100) == FuriStatusOk) {
            app->current_state = ChatStateSending;
            
            furi_hal_subghz_idle();
            furi_delay_ms(50);
            furi_hal_subghz_set_frequency(CHAT_FREQ);
            
            if(furi_hal_subghz_start_async_tx(chat_tx_callback_payload, NULL)) {
                furi_delay_ms(300); // Имитация долгой отправки для теста анимации
                furi_hal_subghz_stop_async_tx();
            }
            
            furi_hal_subghz_idle();
            furi_delay_ms(50);
            furi_hal_subghz_rx();
            app->current_state = ChatStateIdle;
        }
    }
    return 0;
}

static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat v6.1.1");
    
    canvas_set_font(canvas, FontSecondary);
    if(m->state == ChatStateSending) {
        canvas_draw_str(canvas, 2, 32, "STATUS: SENDING...");
        // Анимация "бегущего квадрата"
        int progress = (furi_get_tick() % 100);
        canvas_draw_box(canvas, 2 + progress, 40, 10, 4);
    } else {
        canvas_draw_str(canvas, 2, 32, "STATUS: READY");
        canvas_draw_str(canvas, 2, 45, "Press OK to write");
    }
}

// Правильное обновление модели по таймеру
static void scene_update_timer_callback(void* context) {
    ChatApp* app = context;
    with_view_model(app->main_view, ChatModel* m, {
        m->state = app->current_state;
    }, true); // true автоматически делает commit
}

static void text_input_done(void* ctx) {
    ChatApp* app = ctx;
    ChatMessage msg;
    memset(&msg, 0, sizeof(ChatMessage));
    strncpy(msg.text, app->tx_buf, 63);
    
    furi_message_queue_put(app->tx_queue, &msg, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
}

static uint32_t back_to_main(void* ctx) { UNUSED(ctx); return 0; }

static bool input_callback(InputEvent* event, void* ctx) {
    ChatApp* app = ctx;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(app->current_state == ChatStateIdle) {
            view_dispatcher_switch_to_view(app->view_dispatcher, 1);
        }
        return true;
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

    app->tx_queue = furi_message_queue_alloc(2, sizeof(ChatMessage));
    app->worker_thread = furi_thread_alloc_ex("SubChatWorker", 2048, chat_worker_thread, app);
    furi_thread_start(app->worker_thread);

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLockFree, sizeof(ChatModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, render_callback);
    view_set_input_callback(app->main_view, input_callback);

    app->text_input = text_input_alloc();
    text_input_set_result_callback(app->text_input, text_input_done, app, app->tx_buf, 64, true);
    view_set_previous_callback(text_input_get_view(app->text_input), back_to_main);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_add_view(app->view_dispatcher, 1, text_input_get_view(app->text_input));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    FuriTimer* timer = furi_timer_alloc(scene_update_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(timer, 50); // 20 FPS для плавной анимации

    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    furi_hal_subghz_rx();

    view_dispatcher_run(app->view_dispatcher);

    furi_timer_stop(timer);
    furi_timer_free(timer);
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
