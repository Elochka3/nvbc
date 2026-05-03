#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <string.h>

#define CHAT_FREQ 433920000 

typedef enum {
    ChatEventSendPacket,
} ChatCustomEvent;

typedef struct {
    char last_msg[64];
} ChatModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    char tx_buf[64];
} ChatApp;

// Коллбэк для генерации тестового импульса
static LevelDuration chat_tx_callback_dummy(void* context) {
    UNUSED(context);
    return level_duration_make(true, 500); 
}

// Рендер интерфейса
static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Chat v4.8 [ULN]");
    
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_line(canvas, 0, 14, 128, 14);
    
    canvas_draw_str(canvas, 2, 30, "Status:");
    canvas_draw_str(canvas, 40, 30, m->last_msg);
    
    canvas_draw_line(canvas, 0, 52, 128, 52);
    canvas_draw_str(canvas, 2, 62, "OK: Write Message");
}

// БЕЗОПАСНАЯ ОТПРАВКА (Выполняется в основном потоке)
static void perform_send(ChatApp* app) {
    with_view_model(app->main_view, ChatModel* m, {
        strncpy(m->last_msg, "Sending...", 63);
    }, true);

    // 1. Подготовка чипа
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    
    // 2. Проверка возможности передачи (важно для Unleashed)
    if(furi_hal_subghz_is_tx_allowed(CHAT_FREQ)) {
        if(furi_hal_subghz_start_async_tx(chat_tx_callback_dummy, NULL)) {
            furi_delay_ms(100); // Короткий импульс
            furi_hal_subghz_stop_async_tx();
            
            with_view_model(app->main_view, ChatModel* m, {
                strncpy(m->last_msg, "Sent OK!", 63);
            }, true);
        } else {
             with_view_model(app->main_view, ChatModel* m, {
                strncpy(m->last_msg, "TX Start Error", 63);
            }, true);
        }
    } else {
        with_view_model(app->main_view, ChatModel* m, {
            strncpy(m->last_msg, "Freq Locked!", 63);
        }, true);
    }

    // 3. Возврат в прием
    furi_hal_subghz_rx();
}

static bool chat_custom_event_callback(void* context, uint32_t event) {
    ChatApp* app = context;
    if(event == ChatEventSendPacket) {
        perform_send(app);
        return true;
    }
    return false;
}

static void text_input_done(void* ctx) {
    ChatApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, ChatEventSendPacket);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
}

static uint32_t back_to_main(void* ctx) { UNUSED(ctx); return 0; }

static bool input_callback(InputEvent* event, void* ctx) {
    ChatApp* app = ctx;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_switch_to_view(app->view_dispatcher, 1);
        return true;
    }
    return false;
}

int32_t subghz_chat_app(void* p) {
    UNUSED(p);
    ChatApp* app = malloc(sizeof(ChatApp));
    memset(app, 0, sizeof(ChatApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    
    // Настройка событий
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, chat_custom_event_callback);

    // Главный экран
    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLockFree, sizeof(ChatModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, render_callback);
    view_set_input_callback(app->main_view, input_callback);

    // Ввод текста
    app->text_input = text_input_alloc();
    text_input_set_result_callback(app->text_input, text_input_done, app, app->tx_buf, 64, true);
    view_set_previous_callback(text_input_get_view(app->text_input), back_to_main);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_add_view(app->view_dispatcher, 1, text_input_get_view(app->text_input));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    // Инициализация радио
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    furi_hal_subghz_rx();

    view_dispatcher_run(app->view_dispatcher);

    // Cleanup
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
